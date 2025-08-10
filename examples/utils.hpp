// utils.hpp - Shared utilities for OpenFHE benchmarks
#pragma once

#include <openfhe.h>
#include <iostream>
#include <vector>
#include <random>
#include <string>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <omp.h>

#include <dram_counter.hpp>

using namespace lbcrypto;

// PIN markers for integer operation counting
extern "C" {
    void __attribute__((noinline)) PIN_MARKER_START() { asm volatile(""); }
    void __attribute__((noinline)) PIN_MARKER_END() { asm volatile(""); }
}

// Measurement modes
enum class MeasurementMode {
    LATENCY,
    DRAM,
    PIN
};

// Simple command-line argument parser
class ArgParser {
private:
    std::map<std::string, std::string> args;

public:
    void parse(int argc, char* argv[]) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            size_t pos = arg.find('=');
            if (pos != std::string::npos) {
                std::string key = arg.substr(2, pos - 2);  // Remove --
                std::string value = arg.substr(pos + 1);
                args[key] = value;
            }
        }
    }
    
    std::string getString(const std::string& key, const std::string& defaultVal = "") const {
        auto it = args.find(key);
        return (it != args.end()) ? it->second : defaultVal;
    }
    
    uint32_t getUInt32(const std::string& key, uint32_t defaultVal = 0) const {
        auto it = args.find(key);
        return (it != args.end()) ? std::stoul(it->second) : defaultVal;
    }
    
    bool getBool(const std::string& key, bool defaultVal = false) const {
        auto it = args.find(key);
        return (it != args.end()) ? (it->second == "true") : defaultVal;
    }
    
    bool getDebug() const {
        return getBool("debug", false);
    }
    
    MeasurementMode getMeasurementMode() const {
        std::string mode = getString("measure", "latency");
        if (mode == "dram") return MeasurementMode::DRAM;
        if (mode == "pin") return MeasurementMode::PIN;
        return MeasurementMode::LATENCY;  // default
    }
};

// Benchmark parameters
struct BenchmarkParams {
    uint32_t ringDim;
    uint32_t multDepth;
    uint32_t numDigits;
    bool checkSecurity;
    
    static BenchmarkParams fromArgs(const ArgParser& parser) {
        return {
            .ringDim = parser.getUInt32("ring-dim"),
            .multDepth = parser.getUInt32("mult-depth"),
            .numDigits = parser.getUInt32("num-digits"),
            .checkSecurity = parser.getBool("check-security")
        };
    }
};

// Thread setup
inline void setupThreads(const ArgParser& parser) {
    uint32_t requested = parser.getUInt32("threads", 0);
    
    if (requested > 0) {
        omp_set_num_threads(requested);
    }
}

// Measurement wrapper
class MeasurementSystem {
private:
    MeasurementMode mode;
    DRAMCounter dramCounter;
    bool dramInitialized = false;
    
public:
    MeasurementSystem(MeasurementMode m) : mode(m) {
        if (mode == MeasurementMode::DRAM) {
            dramInitialized = dramCounter.init();
        }
    }
    
    void startDRAM() {
        if (mode == MeasurementMode::DRAM && dramInitialized) {
            dramCounter.start();
        }
    }
    
    void stopDRAM() {
        if (mode == MeasurementMode::DRAM && dramInitialized) {
            dramCounter.stop();
        }
    }
    
    void startPIN() {
        if (mode == MeasurementMode::PIN) {
            PIN_MARKER_START();
        }
    }
    
    void endPIN() {
        if (mode == MeasurementMode::PIN) {
            PIN_MARKER_END();
        }
    }
    
    void printResults() {
        if (mode == MeasurementMode::DRAM && dramInitialized) {
            dramCounter.print_results();
        }
    }
};

// Temporary directory for serialization
class TempDirectory {
private:
    std::string path;
    
public:
    TempDirectory() {
        char tmpTemplate[] = "/tmp/openfhe_bench_XXXXXX";
        char* tmpDir = mkdtemp(tmpTemplate);
        if (tmpDir) {
            path = std::string(tmpDir);
            std::filesystem::create_directories(path + "/data");
        }
    }
    
    ~TempDirectory() {
        if (!path.empty()) {
            std::filesystem::remove_all(path);
        }
    }
    
    bool isValid() const { return !path.empty(); }
    std::string getFilePath(const std::string& filename) const {
        return path + "/" + filename;
    }
};

// Matrix/vector utilities
inline std::vector<std::vector<double>> make_embedded_random_matrix(
    std::size_t matrixDim, std::size_t numSlots) 
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.1, 2.0);
    
    std::vector<std::vector<double>> M(numSlots, std::vector<double>(numSlots, 0.0));
    for (std::size_t i = 0; i < matrixDim; ++i) {
        for (std::size_t j = 0; j < matrixDim; ++j) {
            M[i][j] = dis(gen);
        }
    }
    return M;
}

inline std::vector<double> make_random_input_vector(
    std::size_t matrixDim, std::size_t numSlots) 
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.5, 1.5);
    
    std::vector<double> vec(numSlots, 0.0);
    for (std::size_t i = 0; i < matrixDim; ++i) {
        vec[i] = dis(gen);
    }
    return vec;
}

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
                    diag[i] = M[i][j];  // FIX: Place at position i, not (i+k)
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

// Simple verification - returns bool, prints only if debug
inline bool verifyResult(const std::vector<double>& result, 
                        const std::vector<double>& expected,
                        bool debug = false) 
{
    double maxError = 0.0;
    for (size_t i = 0; i < std::min(result.size(), expected.size()); i++) {
        maxError = std::max(maxError, std::abs(result[i] - expected[i]));
    }
    
    bool passed = (maxError < 1e-6);
    
    if (debug) {
        if (passed) {
            std::cout << "✓ Verification PASSED\n";
        } else {
            std::cout << "✗ Verification FAILED - Max error: " << maxError << "\n";
        }
    }
    
    return passed;
}

// Matrix-vector verification - returns bool, prints only if debug
inline bool verify_matrix_vector_result(
    const std::vector<double>& result,
    const std::vector<std::vector<double>>& M,
    const std::vector<double>& input,
    std::size_t matrixDim,
    bool debug = false) 
{
    std::vector<double> expected(M.size(), 0.0);
    for (std::size_t i = 0; i < matrixDim; ++i) {
        for (std::size_t j = 0; j < matrixDim; ++j) {
            expected[i] += M[i][j] * input[j];
        }
    }
    
    return verifyResult(result, expected, debug);
}

// Rotate vector (matches OpenFHE's EvalRotate direction)
// Positive index = rotate left, negative index = rotate right
inline std::vector<double> rotate(const std::vector<double>& vec, int k) {
    std::size_t n = vec.size();
    if (n == 0) return vec;
    
    // Normalize k to be in range [0, n)
    k = k % (int)n;
    if (k < 0) k += n;
    
    std::vector<double> result(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Rotating left by k means element at position i comes from position (i+k)%n
        result[i] = vec[(i + k) % n];
    }
    return result;
}

// Rotate vector down by k positions (helper for pre-shifting diagonals)
// This is equivalent to rotating right by k, which is rotating left by -k
inline std::vector<double> rotateVectorDown(const std::vector<double>& vec, int k) {
    return rotate(vec, -k);
}

// Convert diagonal index from [0, slots-1] to signed range [-slots/2, slots/2]
// This treats the second half of slots as negative indices
// Example: in 64 slots, index 63 becomes -1 (one step backwards)
int normalizeToSignedIndex(int k, int numSlots) {
    int halfSlots = numSlots / 2;
    if (k <= halfSlots) {
        return k;  // First half stays positive
    } else {
        return k - numSlots;  // Second half becomes negative
    }
}

// Floor division that works correctly for negative numbers
// Regular C++ division truncates toward zero, but we need true floor division
// Example: -5 / 3 = -2 (floor), not -1 (truncation)
int floorDivision(int a, int b) {
    int quotient = a / b;
    int remainder = a % b;
    // Adjust if remainder is nonzero and signs differ
    if (remainder != 0 && ((a < 0) != (b < 0))) {
        quotient--;
    }
    return quotient;
}