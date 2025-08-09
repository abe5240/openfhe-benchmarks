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
    NONE,
    DRAM_ONLY,
    PIN_ONLY
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
    
    MeasurementMode getMeasurementMode() const {
        std::string mode = getString("measure", "none");
        if (mode == "dram") return MeasurementMode::DRAM_ONLY;
        if (mode == "pin") return MeasurementMode::PIN_ONLY;
        return MeasurementMode::NONE;
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
            .ringDim = parser.getUInt32("ring-dim", 8192),
            .multDepth = parser.getUInt32("mult-depth", 4),
            .numDigits = parser.getUInt32("num-digits", 1),
            .checkSecurity = parser.getBool("check-security", false)
        };
    }
};

// Thread setup
inline void setupThreads(const ArgParser& parser) {
    uint32_t threads = parser.getUInt32("threads", 0);
    if (threads > 0) {
        omp_set_num_threads(threads);
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
        if (mode == MeasurementMode::DRAM_ONLY) {
            dramInitialized = dramCounter.init();
        }
    }
    
    void startDRAM() {
        if (mode == MeasurementMode::DRAM_ONLY && dramInitialized) {
            dramCounter.start();
        }
    }
    
    void stopDRAM() {
        if (mode == MeasurementMode::DRAM_ONLY && dramInitialized) {
            dramCounter.stop();
        }
    }
    
    void startPIN() {
        if (mode == MeasurementMode::PIN_ONLY) {
            PIN_MARKER_START();
        }
    }
    
    void endPIN() {
        if (mode == MeasurementMode::PIN_ONLY) {
            PIN_MARKER_END();
        }
    }
    
    void printResults() {
        if (mode == MeasurementMode::DRAM_ONLY && dramInitialized) {
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

// Extract generalized diagonals from matrix
inline std::map<int, std::vector<double>> extract_generalized_diagonals(
    const std::vector<std::vector<double>>& M, std::size_t matrixDim) 
{
    std::size_t numSlots = M.size();
    std::map<int, std::vector<double>> diagonals;
    
    for (std::size_t k = 0; k < numSlots; ++k) {
        std::vector<double> diag(numSlots, 0.0);
        bool hasNonZero = false;
        
        for (std::size_t j = 0; j < matrixDim; ++j) {
            for (std::size_t i = 0; i < matrixDim; ++i) {
                if ((j + numSlots - i) % numSlots == k) {
                    std::size_t idx = (i + k) % numSlots;
                    diag[idx] = M[i][j];
                    if (M[i][j] != 0.0) hasNonZero = true;
                }
            }
        }
        
        if (hasNonZero) {
            diagonals[k] = diag;
        }
    }
    return diagonals;
}

// Simple verification
inline void verifyResult(const std::vector<double>& result, 
                        const std::vector<double>& expected,
                        bool quiet = false) 
{
    if (quiet) return;
    
    double maxError = 0.0;
    for (size_t i = 0; i < std::min(result.size(), expected.size()); i++) {
        maxError = std::max(maxError, std::abs(result[i] - expected[i]));
    }
    
    if (maxError < 1e-6) {
        std::cout << "✓ Verification PASSED\n";
    } else {
        std::cout << "⚠ Max error: " << maxError << "\n";
    }
}

// Matrix-vector verification
inline void verify_matrix_vector_result(
    const std::vector<double>& result,
    const std::vector<std::vector<double>>& M,
    const std::vector<double>& input,
    std::size_t matrixDim,
    bool quiet = false) 
{
    if (quiet) return;
    
    std::vector<double> expected(M.size(), 0.0);
    for (std::size_t i = 0; i < matrixDim; ++i) {
        for (std::size_t j = 0; j < matrixDim; ++j) {
            expected[i] += M[i][j] * input[j];
        }
    }
    
    verifyResult(result, expected, quiet);
}