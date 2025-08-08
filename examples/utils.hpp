// utils.hpp - Shared utilities for benchmarks
#ifndef OPENFHE_BENCHMARKS_UTILS_HPP
#define OPENFHE_BENCHMARKS_UTILS_HPP

#include <iostream>
#include <iomanip>
#include <vector>
#include <random>
#include <cassert>
#include <map>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <dram_counter.hpp>  // IMPORTANT: Add this include!

// =============================================================================
// SECTION 1: PIN INSTRUMENTATION MARKERS
// =============================================================================

extern "C" {
    inline void __attribute__((noinline)) PIN_MARKER_START() { 
        asm volatile(""); 
    }
    inline void __attribute__((noinline)) PIN_MARKER_END() { 
        asm volatile("");
    }
}

// =============================================================================
// SECTION 2: MEASUREMENT MODE CONFIGURATION
// =============================================================================

enum class MeasurementMode {
    NONE,       // No instrumentation (pure performance)
    DRAM_ONLY,  // DRAM counters only (minimal overhead)
    PIN_ONLY,   // PIN markers only (for operation counting)
    ALL         // All measurements (default, has overhead)
};

inline std::string measurementModeToString(MeasurementMode mode) {
    switch (mode) {
        case MeasurementMode::NONE: return "NONE (pure performance)";
        case MeasurementMode::DRAM_ONLY: return "DRAM_ONLY (hardware counters)";
        case MeasurementMode::PIN_ONLY: return "PIN_ONLY (operation counting)";
        case MeasurementMode::ALL: return "ALL (DRAM + PIN markers)";
        default: return "UNKNOWN";
    }
}

// =============================================================================
// SECTION 3: COMMAND-LINE ARGUMENT PARSER
// =============================================================================

class ArgParser {
private:
    std::map<std::string, std::string> args;
    
public:
    void parse(int argc, char* argv[]) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            size_t eq_pos = arg.find('=');
            
            if (eq_pos != std::string::npos) {
                // Format: --key=value
                std::string key = arg.substr(0, eq_pos);
                std::string value = arg.substr(eq_pos + 1);
                
                // Remove leading dashes
                while (!key.empty() && key[0] == '-') {
                    key = key.substr(1);
                }
                
                args[key] = value;
            }
        }
    }
    
    std::string getString(const std::string& key, const std::string& defaultValue) const {
        auto it = args.find(key);
        if (it != args.end()) {
            return it->second;
        }
        return defaultValue;
    }
    
    uint32_t getUInt32(const std::string& key, uint32_t defaultValue) const {
        auto it = args.find(key);
        if (it != args.end()) {
            try {
                return std::stoul(it->second);
            } catch (...) {
                std::cerr << "Warning: Invalid value for --" << key 
                         << ", using default: " << defaultValue << std::endl;
            }
        }
        return defaultValue;
    }
    
    bool getBool(const std::string& key, bool defaultValue) const {
        auto it = args.find(key);
        if (it != args.end()) {
            std::string val = it->second;
            // Convert to lowercase for case-insensitive comparison
            std::transform(val.begin(), val.end(), val.begin(), ::tolower);
            
            // Accept various boolean representations
            if (val == "1" || val == "true" || val == "yes" || val == "on") {
                return true;
            } else if (val == "0" || val == "false" || val == "no" || val == "off") {
                return false;
            } else {
                std::cerr << "Warning: Invalid boolean value for --" << key 
                         << ", using default: " << (defaultValue ? "true" : "false") << std::endl;
            }
        }
        return defaultValue;
    }
    
    MeasurementMode getMeasurementMode() const {
        std::string mode = getString("measure", "all");
        
        // Convert to lowercase
        std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
        
        if (mode == "none") return MeasurementMode::NONE;
        if (mode == "dram") return MeasurementMode::DRAM_ONLY;
        if (mode == "pin") return MeasurementMode::PIN_ONLY;
        if (mode == "all") return MeasurementMode::ALL;
        
        // Default to ALL if unrecognized
        std::cerr << "Warning: Unknown measurement mode '" << mode 
                  << "', using 'all'\n";
        return MeasurementMode::ALL;
    }
    
    bool hasHelp() const {
        return args.find("help") != args.end() || args.find("h") != args.end();
    }
    
    void printUsage(const std::string& programName) const {
        std::cout << "Usage: " << programName << " [options]\n"
                  << "Options:\n"
                  << "  --mult-depth=N      Multiplicative depth (default: 19)\n"
                  << "  --scale-mod-size=N  Scaling modulus size (default: 50)\n"
                  << "  --ring-dim=N        Ring dimension (default: 65536)\n"
                  << "  --check-security=B  Enable security level check (default: false)\n"
                  << "  --measure=MODE      Measurement mode (default: all)\n"
                  << "                      none: No instrumentation\n"
                  << "                      dram: DRAM counters only\n"
                  << "                      pin:  PIN markers only\n"
                  << "                      all:  All measurements\n"
                  << "  --quiet=B           Suppress verbose output (default: false)\n"
                  << "  --skip-verify=B     Skip result verification (default: false)\n"
                  << "  --help              Show this help message\n"
                  << "\nExample:\n"
                  << "  ./" << programName << " --mult-depth=10 --scale-mod-size=40 --ring-dim=32768\n";
    }
};

// =============================================================================
// SECTION 4: BENCHMARK PARAMETER CONFIGURATION
// =============================================================================

struct BenchmarkParams {
    uint32_t multDepth;
    uint32_t scaleModSize;
    uint32_t ringDim;
    bool checkSecurity;
    
    static BenchmarkParams fromArgs(const ArgParser& parser) {
        return {
            .multDepth = parser.getUInt32("mult-depth", 19),
            .scaleModSize = parser.getUInt32("scale-mod-size", 50),
            .ringDim = parser.getUInt32("ring-dim", 65536),
            .checkSecurity = parser.getBool("check-security", false)
        };
    }
    
    void print() const {
        std::cout << "=== Configuration ===" << std::endl;
        std::cout << "Multiplicative depth: " << multDepth << std::endl;
        std::cout << "Scaling modulus size: " << scaleModSize << std::endl;
        std::cout << "Ring dimension: " << ringDim << std::endl;
        std::cout << "Security check: " << (checkSecurity ? "ENABLED (HEStd_128_classic)" : "DISABLED") << std::endl;
        std::cout << std::endl;
    }
};

// =============================================================================
// SECTION 5: TEMPORARY DIRECTORY MANAGEMENT
// =============================================================================

class TempDirectory {
private:
    std::string path;
    bool valid;
    
public:
    explicit TempDirectory(const std::string& prefix = "/tmp/openfhe_bench_") {
        std::vector<char> tmpDir(prefix.begin(), prefix.end());
        tmpDir.insert(tmpDir.end(), {'X', 'X', 'X', 'X', 'X', 'X', '\0'});
        
        if (mkdtemp(tmpDir.data()) != nullptr) {
            path = tmpDir.data();
            valid = true;
            // Respect quiet mode
            if (getenv("QUIET_MODE") == nullptr) {
                std::cout << "Created temporary directory: " << path << std::endl;
            }
        } else {
            valid = false;
            std::cerr << "ERROR: Failed to create temporary directory" << std::endl;
        }
    }
    
    ~TempDirectory() {
        if (valid && !path.empty()) {
            std::string cmd = "rm -rf " + path;
            system(cmd.c_str());
            // Respect quiet mode
            if (getenv("QUIET_MODE") == nullptr) {
                std::cout << "Cleaned up temporary directory: " << path << std::endl;
            }
        }
    }
    
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    
    bool isValid() const { return valid; }
    const std::string& getPath() const { return path; }
    std::string getFilePath(const std::string& filename) const {
        return valid ? path + "/" + filename : "";
    }
};

// =============================================================================
// SECTION 6: PROFILING RESULTS
// =============================================================================

struct ProfilingResults {
    uint64_t integerOps = 0;
    uint64_t dramReadBytes = 0;
    uint64_t dramWriteBytes = 0;
    uint64_t dramTotalBytes = 0;
    double arithmeticIntensity = 0.0;
    bool hasIntegerOps = false;
    bool hasDramData = false;
    
    // Read integer ops from PIN tool output
    void readIntegerOps() {
        std::ifstream file("logs/int_counts.out");
        if (!file.is_open()) return;
        
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("TOTAL counted:") != std::string::npos) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    try {
                        integerOps = std::stoull(line.substr(pos + 1));
                        hasIntegerOps = true;
                    } catch (...) {}
                }
            }
        }
    }
    
    // Read DRAM bytes from dram_counter output
    void readDramBytes() {
        std::ifstream file("logs/dram_counts.out");
        if (!file.is_open()) return;
        
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("DRAM_READ_BYTES=") == 0) {
                dramReadBytes = std::stoull(line.substr(16));
            } else if (line.find("DRAM_WRITE_BYTES=") == 0) {
                dramWriteBytes = std::stoull(line.substr(17));
            } else if (line.find("DRAM_TOTAL_BYTES=") == 0) {
                dramTotalBytes = std::stoull(line.substr(17));
                hasDramData = true;
            }
        }
    }
    
    // Calculate arithmetic intensity
    void calculate() {
        readIntegerOps();
        readDramBytes();
        
        if (hasIntegerOps && hasDramData && dramTotalBytes > 0) {
            arithmeticIntensity = static_cast<double>(integerOps) / dramTotalBytes;
        }
    }
    
    // Print for Python parsing
    void printForPython() const {
        std::cout << "\n=== PROFILING_METRICS ===" << std::endl;
        std::cout << "INTEGER_OPS=" << integerOps << std::endl;
        std::cout << "DRAM_READ_BYTES=" << dramReadBytes << std::endl;
        std::cout << "DRAM_WRITE_BYTES=" << dramWriteBytes << std::endl;
        std::cout << "DRAM_TOTAL_BYTES=" << dramTotalBytes << std::endl;
        std::cout << "ARITHMETIC_INTENSITY=" << std::fixed << std::setprecision(9) 
                  << arithmeticIntensity << std::endl;
        std::cout << "HAS_PIN_DATA=" << (hasIntegerOps ? "true" : "false") << std::endl;
        std::cout << "HAS_DRAM_DATA=" << (hasDramData ? "true" : "false") << std::endl;
        std::cout << "=== END_PROFILING_METRICS ===" << std::endl;
    }
    
    // Print human-readable
    void printHuman() const {
        std::cout << "\n=== Profiling Results ===" << std::endl;
        
        if (hasIntegerOps) {
            std::cout << "Integer operations: " << integerOps << std::endl;
        } else {
            std::cout << "Integer operations: Not measured (PIN not available)" << std::endl;
        }
        
        if (hasDramData) {
            auto formatBytes = [](uint64_t bytes) -> std::string {
                char buf[32];
                if (bytes >= (1ULL << 30)) 
                    sprintf(buf, "%.2f GiB", bytes / double(1ULL << 30));
                else if (bytes >= (1ULL << 20)) 
                    sprintf(buf, "%.2f MiB", bytes / double(1ULL << 20));
                else if (bytes >= (1ULL << 10)) 
                    sprintf(buf, "%.2f KiB", bytes / double(1ULL << 10));
                else 
                    sprintf(buf, "%llu bytes", (unsigned long long)bytes);
                return std::string(buf);
            };
            
            std::cout << "DRAM Read:  " << formatBytes(dramReadBytes) << std::endl;
            std::cout << "DRAM Write: " << formatBytes(dramWriteBytes) << std::endl;
            std::cout << "DRAM Total: " << formatBytes(dramTotalBytes) << std::endl;
        } else {
            std::cout << "DRAM traffic: Not measured" << std::endl;
        }
        
        if (hasIntegerOps && hasDramData && dramTotalBytes > 0) {
            std::cout << "Arithmetic Intensity: " << std::fixed << std::setprecision(6) 
                      << arithmeticIntensity << " ops/byte" << std::endl;
        }
    }
};

// =============================================================================
// SECTION 7: MEASUREMENT SYSTEM (Clean Interface for Benchmarks)
// =============================================================================

class MeasurementSystem {
private:
    static DRAMCounter dram_counter;
    MeasurementMode mode;
    bool quiet;
    
public:
    MeasurementSystem(MeasurementMode m, bool q = false) : mode(m), quiet(q) {}
    
    // Initialize measurement (call at start of main)
    bool initialize() {
        if (mode == MeasurementMode::DRAM_ONLY || mode == MeasurementMode::ALL) {
            if (!dram_counter.init()) {
                if (!quiet) {
                    std::cerr << "Warning: DRAM measurements disabled (try sudo)\n";
                }
                return false;
            }
        }
        return true;
    }
    
    // Start DRAM measurement only
    void startDRAM() {
        if (mode == MeasurementMode::DRAM_ONLY || mode == MeasurementMode::ALL) {
            dram_counter.start();
        }
    }
    
    // Stop DRAM measurement only
    void stopDRAM() {
        if (mode == MeasurementMode::DRAM_ONLY || mode == MeasurementMode::ALL) {
            dram_counter.stop();
            dram_counter.print_results();
        }
    }
    
    // Start measuring (both DRAM and PIN)
    void startMeasuring() {
        if (mode == MeasurementMode::DRAM_ONLY || mode == MeasurementMode::ALL) {
            dram_counter.start();
        }
        if (mode == MeasurementMode::PIN_ONLY || mode == MeasurementMode::ALL) {
            PIN_MARKER_START();
        }
    }
    
    // Stop measuring (both DRAM and PIN)
    void stopMeasuring() {
        if (mode == MeasurementMode::PIN_ONLY || mode == MeasurementMode::ALL) {
            PIN_MARKER_END();
        }
        if (mode == MeasurementMode::DRAM_ONLY || mode == MeasurementMode::ALL) {
            dram_counter.stop();
            dram_counter.print_results();
        }
    }
    
    // Print results (call at end)
    void printResults() {
        if (mode == MeasurementMode::NONE) return;
        
        ProfilingResults results;
        
        // Only read what we measured
        if (mode == MeasurementMode::DRAM_ONLY) {
            results.readDramBytes();
        } else if (mode == MeasurementMode::PIN_ONLY) {
            results.readIntegerOps();
        } else if (mode == MeasurementMode::ALL) {
            results.readIntegerOps();
            results.readDramBytes();
            if (results.hasIntegerOps && results.hasDramData && results.dramTotalBytes > 0) {
                results.arithmeticIntensity = static_cast<double>(results.integerOps) / results.dramTotalBytes;
            }
        }
        
        // Print for Python parsing
        std::cout << "\n=== PROFILING_METRICS ===" << std::endl;
        
        if (mode == MeasurementMode::PIN_ONLY || mode == MeasurementMode::ALL) {
            std::cout << "INTEGER_OPS=" << results.integerOps << std::endl;
        }
        
        if (mode == MeasurementMode::DRAM_ONLY || mode == MeasurementMode::ALL) {
            std::cout << "DRAM_READ_BYTES=" << results.dramReadBytes << std::endl;
            std::cout << "DRAM_WRITE_BYTES=" << results.dramWriteBytes << std::endl;
            std::cout << "DRAM_TOTAL_BYTES=" << results.dramTotalBytes << std::endl;
        }
        
        if (mode == MeasurementMode::ALL) {
            std::cout << "ARITHMETIC_INTENSITY=" << std::fixed << std::setprecision(9) 
                      << results.arithmeticIntensity << std::endl;
        }
        
        std::cout << "HAS_PIN_DATA=" << (results.hasIntegerOps ? "true" : "false") << std::endl;
        std::cout << "HAS_DRAM_DATA=" << (results.hasDramData ? "true" : "false") << std::endl;
        std::cout << "=== END_PROFILING_METRICS ===" << std::endl;
        
        // Human readable if not quiet
        if (!quiet) {
            std::cout << "\n=== Profiling Results ===" << std::endl;
            
            if (results.hasIntegerOps) {
                std::cout << "Integer operations: " << results.integerOps << std::endl;
            }
            
            if (results.hasDramData) {
                auto formatBytes = [](uint64_t bytes) -> std::string {
                    char buf[32];
                    if (bytes >= (1ULL << 30)) 
                        sprintf(buf, "%.2f GiB", bytes / double(1ULL << 30));
                    else if (bytes >= (1ULL << 20)) 
                        sprintf(buf, "%.2f MiB", bytes / double(1ULL << 20));
                    else if (bytes >= (1ULL << 10)) 
                        sprintf(buf, "%.2f KiB", bytes / double(1ULL << 10));
                    else 
                        sprintf(buf, "%llu bytes", (unsigned long long)bytes);
                    return std::string(buf);
                };
                
                std::cout << "DRAM Read:  " << formatBytes(results.dramReadBytes) << std::endl;
                std::cout << "DRAM Write: " << formatBytes(results.dramWriteBytes) << std::endl;
                std::cout << "DRAM Total: " << formatBytes(results.dramTotalBytes) << std::endl;
            }
            
            if (mode == MeasurementMode::ALL && results.hasIntegerOps && results.hasDramData && results.dramTotalBytes > 0) {
                std::cout << "Arithmetic Intensity: " << std::fixed << std::setprecision(6) 
                          << results.arithmeticIntensity << " ops/byte" << std::endl;
            }
        }
    }
    
    // Get mode string for display
    std::string getModeString() const {
        return measurementModeToString(mode);
    }
};

// Static member definition
DRAMCounter MeasurementSystem::dram_counter;

// =============================================================================
// SECTION 8: VECTOR AND MATRIX UTILITIES
// =============================================================================

inline std::vector<double> make_random_input_vector(std::size_t dim, std::size_t numSlots, 
                                                   bool print = true,
                                                   std::size_t maxElementsToShow = 10) {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    
    std::vector<double> inputVec(numSlots, 0.0);
    for (std::size_t i = 0; i < dim; ++i) {
        inputVec[i] = dist(gen);
    }
    
    if (print) {
        std::size_t elementsToShow = std::min(dim, maxElementsToShow);
        std::cout << "\nInput vector (first " << elementsToShow << " elements): ";
        std::cout << std::fixed << std::setprecision(3);
        for (std::size_t i = 0; i < elementsToShow; ++i) {
            std::cout << std::setw(7) << inputVec[i] << " ";
        }
        if (dim > elementsToShow) std::cout << "...";
        std::cout << "\n\n";
    }
    
    return inputVec;
}

// Matrix utilities
template<typename T>
struct Matrix {
    std::size_t rows, cols;
    std::vector<T> data;
    Matrix(std::size_t r, std::size_t c)
      : rows(r), cols(c), data(r*c) {}
    T*       operator[](std::size_t i)       { assert(i < rows); return data.data() + i*cols; }
    const T* operator[](std::size_t i) const { assert(i < rows); return data.data() + i*cols; }
};

// Create embedded random matrix for diagonal methods
inline Matrix<double> make_embedded_random_matrix(std::size_t dim, std::size_t numSlots) {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    
    Matrix<double> M(numSlots, numSlots);
    
    // Zero entire matrix first
    for (std::size_t i = 0; i < numSlots; ++i) {
        for (std::size_t j = 0; j < numSlots; ++j) {
            M[i][j] = 0.0;
        }
    }
    
    // Fill the actual dim×dim submatrix with random values
    for (std::size_t i = 0; i < dim; ++i) {
        for (std::size_t j = 0; j < dim; ++j) {
            M[i][j] = dist(gen);
        }
    }
    
    return M;
}

// Print matrix (for debugging)
inline void print_matrix(const Matrix<double>& M, std::size_t maxDim = 10) {
    std::size_t showDim = std::min(M.rows, maxDim);
    std::cout << "Matrix (first " << showDim << "×" << showDim << " elements):\n";
    std::cout << std::fixed << std::setprecision(3);
    for (std::size_t i = 0; i < showDim; ++i) {
        for (std::size_t j = 0; j < showDim; ++j) {
            std::cout << std::setw(7) << M[i][j] << " ";
        }
        if (M.cols > showDim) std::cout << "...";
        std::cout << "\n";
    }
    if (M.rows > showDim) std::cout << "...\n";
    std::cout << "\n";
}

// Extract generalized diagonals for diagonal methods
inline std::map<int, std::vector<double>> extract_generalized_diagonals(
    const Matrix<double>& M, std::size_t actualDim) {
    
    std::map<int, std::vector<double>> diagonals;
    std::size_t numSlots = M.rows;
    
    // For each possible diagonal offset k
    for (std::size_t k = 0; k < numSlots; ++k) {
        std::vector<double> diag(numSlots, 0.0);
        bool hasNonZero = false;
        
        // Fill the diagonal
        for (std::size_t i = 0; i < actualDim; ++i) {
            std::size_t j = (i + k) % numSlots;
            if (j < actualDim) {
                diag[i] = M[i][j];
                if (std::abs(diag[i]) > 1e-10) {
                    hasNonZero = true;
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

// Rotate vector for BSGS methods
inline std::vector<double> rotateVectorDown(const std::vector<double>& vec, int positions) {
    std::size_t n = vec.size();
    positions = positions % n;
    if (positions < 0) positions += n;
    
    std::vector<double> result(n);
    for (std::size_t i = 0; i < n; ++i) {
        result[(i + positions) % n] = vec[i];
    }
    return result;
}

// Verify matrix-vector multiplication result
inline void verify_matrix_vector_result(const std::vector<double>& result,
                                       const Matrix<double>& M,
                                       const std::vector<double>& input,
                                       std::size_t actualDim,
                                       double tolerance = 1e-3) {
    std::cout << "Verifying result...\n";
    
    // Compute expected result
    std::vector<double> expected(M.rows, 0.0);
    for (std::size_t i = 0; i < actualDim; ++i) {
        for (std::size_t j = 0; j < actualDim; ++j) {
            expected[i] += M[i][j] * input[j];
        }
    }
    
    // Compare
    bool correct = true;
    double maxError = 0.0;
    for (std::size_t i = 0; i < actualDim; ++i) {
        double error = std::abs(result[i] - expected[i]);
        maxError = std::max(maxError, error);
        if (error > tolerance) {
            correct = false;
            if (i < 5) {  // Show first few errors
                std::cout << "  Position " << i << ": expected " 
                         << expected[i] << ", got " << result[i] 
                         << " (error: " << error << ")\n";
            }
        }
    }
    
    if (correct) {
        std::cout << "✓ Result verified (max error: " << maxError << ")\n";
    } else {
        std::cout << "✗ Result incorrect (max error: " << maxError 
                 << ", tolerance: " << tolerance << ")\n";
    }
}

#endif // OPENFHE_BENCHMARKS_UTILS_HPP