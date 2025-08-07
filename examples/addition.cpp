// examples/addition.cpp - Minimal benchmark for homomorphic addition
#include <openfhe.h>
#include <dram_counter.hpp>
#include <iostream>
#include <vector>
#include <fstream>

// Headers needed for serialization
#include <ciphertext-ser.h>
#include <cryptocontext-ser.h>
#include <key/key-ser.h>
#include <scheme/ckksrns/ckksrns-ser.h>

extern "C" {
    void __attribute__((noinline)) PIN_MARKER_START() { 
        asm volatile(""); 
    }
    void __attribute__((noinline)) PIN_MARKER_END() { 
        asm volatile("");
    }
}

using namespace lbcrypto;

// Global DRAM counter
static DRAMCounter g_dram_counter;

int main() {
    // Initialize DRAM counter
    if (!g_dram_counter.init()) {
        std::cerr << "Warning: DRAM measurements disabled\n";
    }

    // ============================================
    // SETUP: Configure CKKS parameters
    // ============================================
    
    uint32_t multDepth    = 19;
    uint32_t scaleModSize = 50;
    uint32_t ringDim      = 65536;
    
    CCParams<CryptoContextCKKSRNS> params;
    params.SetMultiplicativeDepth(multDepth);
    params.SetScalingModSize(scaleModSize);
    params.SetRingDim(ringDim);
    //params.SetSecurityLevel(HEStd_128_classic);
    
    // Create cryptocontext
    CryptoContext<DCRTPoly> cc = GenCryptoContext(params);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    std::cout << "=== Addition Benchmark ===" << std::endl;

    // Generate key pair (only need public key for encryption and secret for verification)
    auto keyPair = cc->KeyGen();
    // Note: No EvalMultKeyGen or EvalRotateKeyGen needed for addition!

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

    // Serialize input ciphertexts
    std::cout << "Serializing input ciphertexts..." << std::endl;
    
    if (!Serial::SerializeToFile("data/cipher1.bin", cipher1, SerType::BINARY)) {
        std::cerr << "Failed to serialize ciphertext 1" << std::endl;
        return 1;
    }
    
    if (!Serial::SerializeToFile("data/cipher2.bin", cipher2, SerType::BINARY)) {
        std::cerr << "Failed to serialize ciphertext 2" << std::endl;
        return 1;
    }
    
    std::cout << "Serialization complete\n" << std::endl;

    // Clear ciphertexts from memory to ensure we're loading from disk
    //cipher1.reset();
    cipher2.reset();

    // ============================================
    // PROFILED COMPUTATION
    // ============================================
    
    std::cout << "Starting profiled addition...\n" << std::endl;


    volatile uint64_t* a = new uint64_t(3);   // load value = 3
    volatile uint64_t* b = new uint64_t(5);   // load value = 5
    volatile uint64_t* c = new uint64_t(0);   // destination


    // Start DRAM traffic measurement (includes all I/O and computation)
    g_dram_counter.start();

    // Load input ciphertexts from disk
    // if (!Serial::DeserializeFromFile("data/cipher1.bin", c1Loaded, SerType::BINARY)) {
    //     std::cerr << "Failed to load ciphertext 1" << std::endl;
    //     return 1;
    // }

    //Ciphertext<DCRTPoly> c1Loaded = cipher1->Clone(); // , c2Loaded;


    // if (!Serial::DeserializeFromFile("data/cipher2.bin", c2Loaded, SerType::BINARY)) {
    //     std::cerr << "Failed to load ciphertext 2" << std::endl;
    //     return 1;
    // }
    
    // Start integer operation counting
    PIN_MARKER_START();


    *c = *a + *b;   
    //int* arr3 = new int[10000]();
    
    // Perform addition (no key switching needed!)
    //Ciphertext cipherResult; /// = cc->EvalAdd(c1Loaded, c2Loaded);
    
    // Stop integer operation counting
    PIN_MARKER_END();
    
    delete a; delete b; delete c;
    
    // Serialize and save output ciphertext
    // if (!Serial::SerializeToFile("data/result.bin", cipherResult, SerType::BINARY)) {
    //     std::cerr << "Failed to save result ciphertext" << std::endl;
    //     return 1;
    // }
    
    // Stop DRAM traffic measurement
    g_dram_counter.stop();
    
    // Print DRAM results
    g_dram_counter.print_results(true);
    
    // ============================================
    // VERIFICATION: Decrypt and check result
    // ============================================
    
    // std::cout << "\nDecrypting result..." << std::endl;
    
    // Plaintext result;
    // cc->Decrypt(keyPair.secretKey, cipherResult, &result);
    // result->SetLength(vec1.size());
    
    // std::cout.precision(8);
    // std::cout << "Result: " << result << std::endl;
    
    // // Show expected values
    // std::cout << "Expected: (";
    // for (size_t i = 0; i < vec1.size(); i++) {
    //     std::cout << vec1[i] + vec2[i];
    //     if (i < vec1.size() - 1) std::cout << ", ";
    // }
    // std::cout << ")" << std::endl;
    
    return 0;
}