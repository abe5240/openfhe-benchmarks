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
// SECTION 2: COMMAND-LINE ARGUMENT PARSER
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
                  << "  --help              Show this help message\n"
                  << "\nExample:\n"
                  << "  ./" << programName << " --mult-depth=10 --scale-mod-size=40 --ring-dim=32768\n";
    }
};

// =============================================================================
// SECTION 3: BENCHMARK PARAMETER CONFIGURATION
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
// SECTION 4: TEMPORARY DIRECTORY MANAGEMENT
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
            std::cout << "Created temporary directory: " << path << std::endl;
        } else {
            valid = false;
            std::cerr << "ERROR: Failed to create temporary directory" << std::endl;
        }
    }
    
    ~TempDirectory() {
        if (valid && !path.empty()) {
            std::string cmd = "rm -rf " + path;
            system(cmd.c_str());
            std::cout << "Cleaned up temporary directory: " << path << std::endl;
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
// SECTION 5: PROFILING RESULTS
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
// SECTION 6: VECTOR AND MATRIX UTILITIES
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

// Matrix and other utilities remain the same...
template<typename T>
struct Matrix {
    std::size_t rows, cols;
    std::vector<T> data;
    Matrix(std::size_t r, std::size_t c)
      : rows(r), cols(c), data(r*c) {}
    T*       operator[](std::size_t i)       { assert(i < rows); return data.data() + i*cols; }
    const T* operator[](std::size_t i) const { assert(i < rows); return data.data() + i*cols; }
};

// Rest of your matrix utilities here...

#endif // OPENFHE_BENCHMARKS_UTILS_HPP
