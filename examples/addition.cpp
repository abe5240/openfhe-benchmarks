// examples/addition.cpp - Minimal benchmark for homomorphic addition
#include <openfhe.h>
#include <dram_counter.hpp>
#include "utils.hpp"
#include <iostream>
#include <vector>

// Headers needed for serialization
#include <ciphertext-ser.h>
#include <cryptocontext-ser.h>
#include <key/key-ser.h>
#include <scheme/ckksrns/ckksrns-ser.h>

using namespace lbcrypto;

// Global DRAM counter
static DRAMCounter g_dram_counter;

int main(int argc, char* argv[]) {
    // Parse command-line arguments
    ArgParser parser;
    parser.parse(argc, argv);
    
    if (parser.hasHelp()) {
        parser.printUsage("addition");
        return 0;
    }
    
    // Initialize DRAM counter
    if (!g_dram_counter.init()) {
        std::cerr << "Warning: DRAM measurements disabled (try sudo)\n";
    }

    // ============================================
    // SETUP: Configure CKKS parameters
    // ============================================
    
    BenchmarkParams params = BenchmarkParams::fromArgs(parser);
    params.print();
    
    CCParams<CryptoContextCKKSRNS> ccParams;
    ccParams.SetMultiplicativeDepth(params.multDepth);
    ccParams.SetScalingModSize(params.scaleModSize);
    ccParams.SetRingDim(params.ringDim);
    
    if (params.checkSecurity) {
        ccParams.SetSecurityLevel(HEStd_128_classic);
    } else {
        ccParams.SetSecurityLevel(HEStd_NotSet);
    }
    
    // Create cryptocontext
    CryptoContext<DCRTPoly> cc = GenCryptoContext(ccParams);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    std::cout << "=== Addition Benchmark ===" << std::endl;

    // Generate key pair
    auto keyPair = cc->KeyGen();

    // ============================================
    // CREATE AND SERIALIZE INPUT CIPHERTEXTS
    // ============================================
    
    std::vector<double> vec1 = {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7};
    std::vector<double> vec2 = {2.0, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7};
    
    // Encode as plaintexts
    Plaintext ptxt1 = cc->MakeCKKSPackedPlaintext(vec1);
    Plaintext ptxt2 = cc->MakeCKKSPackedPlaintext(vec2);
    
    std::cout << "Input 1: " << ptxt1 << std::endl;
    std::cout << "Input 2: " << ptxt2 << std::endl;

    // Encrypt
    auto cipher1 = cc->Encrypt(keyPair.publicKey, ptxt1);
    auto cipher2 = cc->Encrypt(keyPair.publicKey, ptxt2);

    // Create temporary directory
    TempDirectory tempDir;
    if (!tempDir.isValid()) {
        return 1;
    }

    // Serialize ciphertexts explicitly (keeping it clear what's happening)
    std::cout << "Serializing input ciphertexts..." << std::endl;
    
    std::string cipher1Path = tempDir.getFilePath("cipher1.bin");
    std::string cipher2Path = tempDir.getFilePath("cipher2.bin");
    std::string resultPath = tempDir.getFilePath("result.bin");
    
    if (!Serial::SerializeToFile(cipher1Path, cipher1, SerType::BINARY)) {
        std::cerr << "Failed to serialize ciphertext 1" << std::endl;
        return 1;
    }
    
    if (!Serial::SerializeToFile(cipher2Path, cipher2, SerType::BINARY)) {
        std::cerr << "Failed to serialize ciphertext 2" << std::endl;
        return 1;
    }
    
    std::cout << "Serialization complete\n" << std::endl;

    // Clear ciphertexts from memory to ensure we're loading from disk
    cipher1.reset();
    cipher2.reset();

    // ============================================
    // PROFILED COMPUTATION
    // ============================================
    
    std::cout << "Starting profiled addition...\n" << std::endl;

    // Start DRAM traffic measurement
    g_dram_counter.start();

    // Load input ciphertexts from disk
    Ciphertext<DCRTPoly> c1Loaded, c2Loaded;
    
    if (!Serial::DeserializeFromFile(cipher1Path, c1Loaded, SerType::BINARY)) {
        std::cerr << "Failed to load ciphertext 1" << std::endl;
        return 1;
    }

    if (!Serial::DeserializeFromFile(cipher2Path, c2Loaded, SerType::BINARY)) {
        std::cerr << "Failed to load ciphertext 2" << std::endl;
        return 1;
    }
    
    // Start integer operation counting (PIN will count between these markers)
    PIN_MARKER_START();
    
    // Perform addition - the actual FHE operation!
    auto cipherResult = cc->EvalAdd(c1Loaded, c2Loaded);
    
    // Stop integer operation counting
    PIN_MARKER_END();
        
    // Serialize result
    if (!Serial::SerializeToFile(resultPath, cipherResult, SerType::BINARY)) {
        std::cerr << "Failed to save result ciphertext" << std::endl;
        return 1;
    }
    
    // Stop DRAM traffic measurement
    g_dram_counter.stop();
    
    // Print DRAM results (saves to logs/dram_counts.out)
    g_dram_counter.print_results();
    
    // ============================================
    // COLLECT AND PRINT PROFILING METRICS
    // ============================================
    
    ProfilingResults results;
    results.calculate();  // Reads from logs/int_counts.out and logs/dram_counts.out
    
    // Print for Python parsing
    results.printForPython();
    
    // Also print human-readable
    results.printHuman();
    
    // ============================================
    // VERIFICATION: Decrypt and check result
    // ============================================
    
    std::cout << "\nDecrypting result..." << std::endl;
    
    Plaintext result;
    cc->Decrypt(keyPair.secretKey, cipherResult, &result);
    result->SetLength(vec1.size());
    
    std::cout.precision(8);
    std::cout << "Result: " << result << std::endl;
    
    // Show expected values
    std::cout << "Expected: (";
    for (size_t i = 0; i < vec1.size(); i++) {
        std::cout << vec1[i] + vec2[i];
        if (i < vec1.size() - 1) std::cout << ", ";
    }
    std::cout << ")" << std::endl;
    
    // TempDirectory automatically cleaned up here
    return 0;
}