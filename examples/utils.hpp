// utils.hpp - Shared utilities for OpenFHE benchmarks
#pragma once

#include <openfhe.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <string>
#include <map>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <omp.h>

// Include DRAM counter if available
#if __has_include(<dram_counter.hpp>)
#include <dram_counter.hpp>
#else
#include "dram_counter.hpp"
#endif

using namespace lbcrypto;

// ============================================
// PIN MARKERS FOR INTEGER OPERATION COUNTING
// ============================================

extern "C" {
    void __attribute__((noinline)) PIN_MARKER_START() { 
        asm volatile(""); 
    }
    void __attribute__((noinline)) PIN_MARKER_END() { 
        asm volatile("");
    }
}

// ============================================
// MEASUREMENT MODES
// ============================================

enum class MeasurementMode {
    NONE,       // No measurement
    DRAM_ONLY,  // Only DRAM traffic
    PIN_ONLY,   // Only PIN instrumentation
    ALL         // Both DRAM and PIN
};

// ============================================
// COMMAND-LINE ARGUMENT PARSER
// ============================================

class ArgParser {
private:
    std::map<std::string, std::string> args;
    bool helpRequested = false;

public:
    void parse(int argc, char* argv[]) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            
            if (arg == "--help" || arg == "-h") {
                helpRequested = true;
                continue;
            }
            
            size_t pos = arg.find('=');
            if (pos != std::string::npos) {
                std::string key = arg.substr(2, pos - 2);  // Remove --
                std::string value = arg.substr(pos + 1);
                args[key] = value;
            }
        }
    }
    
    bool hasHelp() const { return helpRequested; }
    
    std::string getString(const std::string& key, const std::string& defaultVal = "") const {
        auto it = args.find(key);
        return (it != args.end()) ? it->second : defaultVal;
    }
    
    uint32_t getUInt32(const std::string& key, uint32_t defaultVal = 0) const {
        auto it = args.find(key);
        if (it != args.end()) {
            try {
                return static_cast<uint32_t>(std::stoul(it->second));
            } catch (...) {
                return defaultVal;
            }
        }
        return defaultVal;
    }
    
    bool getBool(const std::string& key, bool defaultVal = false) const {
        auto it = args.find(key);
        if (it != args.end()) {
            std::string val = it->second;
            return (val == "true" || val == "1" || val == "yes");
        }
        return defaultVal;
    }
    
    MeasurementMode getMeasurementMode() const {
        std::string mode = getString("measure", "dram");
        if (mode == "none") return MeasurementMode::NONE;
        if (mode == "dram") return MeasurementMode::DRAM_ONLY;
        if (mode == "pin") return MeasurementMode::PIN_ONLY;
        if (mode == "all") return MeasurementMode::ALL;
        return MeasurementMode::DRAM_ONLY;  // Default
    }
    
    void printUsage(const std::string& programName) const {
        std::cout << "Usage: " << programName << " [options]\n\n";
        std::cout << "Options:\n";
        std::cout << "  --ring-dim=N          Ring dimension (default: 65536)\n";
        std::cout << "  --mult-depth=N        Multiplicative depth (default: 19)\n";
        std::cout << "  --scale-mod-size=N    Scaling modulus size (default: 50)\n";
        std::cout << "  --check-security=BOOL Enable security check (default: false)\n";
        std::cout << "  --threads=N           Number of OpenMP threads (default: system)\n";
        std::cout << "  --measure=MODE        Measurement mode: none|dram|pin|all (default: dram)\n";
        std::cout << "  --quiet=BOOL          Suppress output (default: false)\n";
        std::cout << "  --skip-verify=BOOL    Skip result verification (default: false)\n";
        std::cout << "  --help                Show this help message\n";
    }
};

// ============================================
// BENCHMARK PARAMETERS
// ============================================

struct BenchmarkParams {
    uint32_t ringDim;
    uint32_t multDepth;
    uint32_t scaleModSize;
    bool checkSecurity;
    
    static BenchmarkParams fromArgs(const ArgParser& parser) {
        return {
            .ringDim = parser.getUInt32("ring-dim", 65536),
            .multDepth = parser.getUInt32("mult-depth", 19),
            .scaleModSize = parser.getUInt32("scale-mod-size", 50),
            .checkSecurity = parser.getBool("check-security", false)
        };
    }
    
    void print() const {
        std::cout << "\n=== Configuration ===" << std::endl;
        std::cout << "Multiplicative depth: " << multDepth << std::endl;
        std::cout << "Scaling modulus size: " << scaleModSize << std::endl;
        std::cout << "Ring dimension: " << ringDim << std::endl;
        std::cout << "Security check: " << (checkSecurity ? "ENABLED" : "DISABLED") << std::endl;
    }
};

// ============================================
// THREAD MANAGEMENT
// ============================================

class ThreadManager {
private:
    uint32_t requestedThreads;
    uint32_t actualThreads;
    bool quiet;
    
public:
    ThreadManager(const ArgParser& parser) 
        : requestedThreads(parser.getUInt32("threads", 0))
        , actualThreads(0)
        , quiet(parser.getBool("quiet", false)) {}
    
    void initialize() {
        // Set threads if specified
        if (requestedThreads > 0) {
            omp_set_num_threads(requestedThreads);
            if (!quiet) {
                std::cout << "OpenMP threads set to: " << requestedThreads << std::endl;
            }
        }
        
        // Report actual thread count
        if (!quiet) {
            #pragma omp parallel
            {
                #pragma omp single
                {
                    actualThreads = omp_get_num_threads();
                    std::cout << "OpenMP using " << actualThreads << " threads" << std::endl;
                }
            }
        } else {
            // Still get the actual count even if quiet
            #pragma omp parallel
            {
                #pragma omp single
                {
                    actualThreads = omp_get_num_threads();
                }
            }
        }
    }
    
    uint32_t getActualThreads() const { return actualThreads; }
    uint32_t getRequestedThreads() const { return requestedThreads; }
};

// ============================================
// MEASUREMENT SYSTEM
// ============================================

class MeasurementSystem {
private:
    MeasurementMode mode;
    bool quiet;
    DRAMCounter dramCounter;
    bool dramInitialized;
    
public:
    MeasurementSystem(MeasurementMode m, bool q = false) 
        : mode(m), quiet(q), dramInitialized(false) {}
    
    void initialize() {
        if (mode == MeasurementMode::DRAM_ONLY || mode == MeasurementMode::ALL) {
            dramInitialized = dramCounter.init();
            if (!dramInitialized && !quiet) {
                std::cerr << "Warning: DRAM counter initialization failed\n";
            }
        }
    }
    
    void startDRAM() {
        if ((mode == MeasurementMode::DRAM_ONLY || mode == MeasurementMode::ALL) && dramInitialized) {
            dramCounter.start();
        }
    }
    
    void stopDRAM() {
        if ((mode == MeasurementMode::DRAM_ONLY || mode == MeasurementMode::ALL) && dramInitialized) {
            dramCounter.stop();
        }
    }
    
    void startPIN() {
        if (mode == MeasurementMode::PIN_ONLY || mode == MeasurementMode::ALL) {
            PIN_MARKER_START();
        }
    }
    
    void endPIN() {
        if (mode == MeasurementMode::PIN_ONLY || mode == MeasurementMode::ALL) {
            PIN_MARKER_END();
        }
    }
    
    void printResults() {
        if ((mode == MeasurementMode::DRAM_ONLY || mode == MeasurementMode::ALL) && dramInitialized) {
            // In quiet mode, output machine-readable format for parsing
            // In normal mode, output human-readable format
            dramCounter.print_results(!quiet);
        }
    }
    
    std::string getModeString() const {
        switch(mode) {
            case MeasurementMode::NONE: return "None";
            case MeasurementMode::DRAM_ONLY: return "DRAM only";
            case MeasurementMode::PIN_ONLY: return "PIN only";
            case MeasurementMode::ALL: return "DRAM + PIN";
            default: return "Unknown";
        }
    }
};

// ============================================
// TEMPORARY DIRECTORY MANAGEMENT
// ============================================

class TempDirectory {
private:
    std::string path;
    bool valid;
    
public:
    TempDirectory() : valid(false) {
        // Create unique temp directory
        char tmpTemplate[] = "/tmp/openfhe_bench_XXXXXX";
        char* tmpDir = mkdtemp(tmpTemplate);
        
        if (tmpDir) {
            path = std::string(tmpDir);
            valid = true;
            
            // Also create a data subdirectory for compatibility
            std::filesystem::create_directories(path + "/data");
        }
    }
    
    ~TempDirectory() {
        if (valid) {
            // Clean up temp directory
            try {
                std::filesystem::remove_all(path);
            } catch (...) {
                // Ignore cleanup errors
            }
        }
    }
    
    bool isValid() const { return valid; }
    
    std::string getPath() const { return path; }
    
    std::string getFilePath(const std::string& filename) const {
        return path + "/" + filename;
    }
    
    std::string getDataPath(const std::string& filename) const {
        return path + "/data/" + filename;
    }
};

// ============================================
// MATRIX AND VECTOR UTILITIES
// ============================================

// Create a random matrix embedded in a larger slot array
std::vector<std::vector<double>> make_embedded_random_matrix(
    std::size_t matrixDim, 
    std::size_t numSlots) 
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.1, 2.0);
    
    std::vector<std::vector<double>> M(numSlots, std::vector<double>(numSlots, 0.0));
    
    // Fill only the matrixDim x matrixDim submatrix
    for (std::size_t i = 0; i < matrixDim; ++i) {
        for (std::size_t j = 0; j < matrixDim; ++j) {
            M[i][j] = dis(gen);
        }
    }
    
    return M;
}

// Create a random input vector
std::vector<double> make_random_input_vector(
    std::size_t matrixDim, 
    std::size_t numSlots) 
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.5, 1.5);
    
    std::vector<double> vec(numSlots, 0.0);
    
    // Fill only the first matrixDim elements
    for (std::size_t i = 0; i < matrixDim; ++i) {
        vec[i] = dis(gen);
    }
    
    return vec;
}

// Print matrix (showing only non-zero portion)
void print_matrix(const std::vector<std::vector<double>>& M, std::size_t matrixDim) {
    std::cout << "Matrix (showing " << matrixDim << "×" << matrixDim << " non-zero portion):\n";
    std::cout << std::fixed << std::setprecision(2);
    
    for (std::size_t i = 0; i < std::min(matrixDim, std::size_t(5)); ++i) {
        for (std::size_t j = 0; j < std::min(matrixDim, std::size_t(5)); ++j) {
            std::cout << std::setw(6) << M[i][j] << " ";
        }
        if (matrixDim > 5) {
            std::cout << "...";
        }
        std::cout << "\n";
    }
    if (matrixDim > 5) {
        std::cout << "   ...\n";
    }
    std::cout << std::endl;
}

// Extract generalized diagonals from matrix
std::map<int, std::vector<double>> extract_generalized_diagonals(
    const std::vector<std::vector<double>>& M, 
    std::size_t matrixDim) 
{
    std::size_t numSlots = M.size();
    std::map<int, std::vector<double>> diagonals;
    
    // For each possible diagonal offset k
    for (std::size_t k = 0; k < numSlots; ++k) {
        std::vector<double> diag(numSlots, 0.0);
        bool hasNonZero = false;
        
        // Extract the k-th generalized diagonal
        for (std::size_t j = 0; j < matrixDim; ++j) {
            for (std::size_t i = 0; i < matrixDim; ++i) {
                // Generalized diagonal: elements where (j - i) mod numSlots = k
                if ((j + numSlots - i) % numSlots == k) {
                    std::size_t idx = (i + k) % numSlots;
                    diag[idx] = M[i][j];
                    if (M[i][j] != 0.0) {
                        hasNonZero = true;
                    }
                }
            }
        }
        
        // Only store non-empty diagonals
        if (hasNonZero) {
            diagonals[k] = diag;
        }
    }
    
    return diagonals;
}

// Rotate vector down by k positions
std::vector<double> rotateVectorDown(const std::vector<double>& vec, int k) {
    std::size_t n = vec.size();
    k = k % n;  // Handle k > n
    if (k < 0) k += n;  // Handle negative k
    
    std::vector<double> result(n);
    for (std::size_t i = 0; i < n; ++i) {
        result[(i + k) % n] = vec[i];
    }
    return result;
}

// Verify matrix-vector multiplication result
void verify_matrix_vector_result(
    const std::vector<double>& result,
    const std::vector<std::vector<double>>& M,
    const std::vector<double>& input,
    std::size_t matrixDim,
    double tolerance = 0.001) 
{
    std::cout << "Verifying result...\n";
    
    // Compute expected result
    std::vector<double> expected(M.size(), 0.0);
    for (std::size_t i = 0; i < matrixDim; ++i) {
        for (std::size_t j = 0; j < matrixDim; ++j) {
            expected[i] += M[i][j] * input[j];
        }
    }
    
    // Compare first few elements
    std::cout << "First 5 elements comparison:\n";
    std::cout << std::fixed << std::setprecision(6);
    
    double maxError = 0.0;
    for (std::size_t i = 0; i < std::min(matrixDim, std::size_t(5)); ++i) {
        double error = std::abs(result[i] - expected[i]);
        maxError = std::max(maxError, error);
        
        std::cout << "  [" << i << "] Result: " << result[i] 
                  << ", Expected: " << expected[i]
                  << ", Error: " << error << "\n";
    }
    
    std::cout << "Maximum error: " << maxError << "\n";
    
    if (maxError < tolerance) {
        std::cout << "✓ Verification PASSED\n";
    } else {
        std::cout << "✗ Verification FAILED (error exceeds tolerance)\n";
    }
}

// ============================================
// RESULT VERIFICATION AND DISPLAY
// ============================================

// Simple function to verify and pretty-print results
void verifyResult(const std::vector<double>& result, 
                  const std::vector<double>& expected,
                  size_t elementsToShow = 5,
                  int precision = 3) 
{
    std::cout << "\n=== Verification ===" << std::endl;
    
    // Set precision for printing
    std::cout << std::fixed << std::setprecision(precision);
    
    // Print first N elements
    size_t showCount = std::min(elementsToShow, std::min(result.size(), expected.size()));
    
    std::cout << "Result:   (";
    for (size_t i = 0; i < showCount; i++) {
        std::cout << result[i];
        if (i < showCount - 1) std::cout << ", ";
    }
    if (result.size() > showCount) std::cout << ", ...";
    std::cout << ")" << std::endl;
    
    std::cout << "Expected: (";
    for (size_t i = 0; i < showCount; i++) {
        std::cout << expected[i];
        if (i < showCount - 1) std::cout << ", ";
    }
    if (expected.size() > showCount) std::cout << ", ...";
    std::cout << ")" << std::endl;
    
    // Calculate error metrics over all elements
    double maxError = 0.0;
    double totalError = 0.0;
    size_t compareCount = std::min(result.size(), expected.size());
    
    for (size_t i = 0; i < compareCount; i++) {
        double error = std::abs(result[i] - expected[i]);
        maxError = std::max(maxError, error);
        totalError += error;
    }
    double avgError = totalError / compareCount;
    
    // Print error metrics
    std::cout << "\nError Metrics:" << std::endl;
    std::cout << "  Average error: " << std::scientific << avgError << std::endl;
    std::cout << "  Maximum error: " << std::scientific << maxError << std::endl;
    
    // Simple pass/fail
    if (maxError < 1e-6) {
        std::cout << "\n✓ Verification PASSED" << std::fixed << std::endl;
    } else {
        std::cout << "\n⚠ Warning: Larger error than expected" << std::fixed << std::endl;
    }
}