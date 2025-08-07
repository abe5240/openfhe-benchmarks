// examples/rotation.cpp - Minimal benchmark for homomorphic rotation
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
    uint32_t numDigits    = 2;
    uint32_t scaleModSize = 50;
    uint32_t ringDim      = 65536;
    
    CCParams<CryptoContextCKKSRNS> params;
    params.SetMultiplicativeDepth(multDepth);
    params.SetScalingModSize(scaleModSize);
    params.SetRingDim(ringDim);
    params.SetScalingTechnique(FLEXIBLEAUTO);
    params.SetKeySwitchTechnique(HYBRID);
    params.SetNumLargeDigits(numDigits);
    params.SetSecurityLevel(HEStd_128_classic);
    
    // Create cryptocontext
    CryptoContext<DCRTPoly> cc = GenCryptoContext(params);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    std::cout << "=== Rotation Benchmark ===" << std::endl;

    // Generate key pair
    auto keyPair = cc->KeyGen();
    
    // Generate rotation keys for specific indices
    // Let's rotate by 1 position (you can change this)
    int rotationIndex = 1;
    std::vector<int32_t> rotationIndices = {rotationIndex};
    cc->EvalRotateKeyGen(keyPair.secretKey, rotationIndices);

    // ============================================
    // SERIALIZE ROTATION KEY TO DISK
    // ============================================
    
    std::cout << "Serializing rotation key for index " << rotationIndex << "..." << std::endl;
    
    std::ofstream rotKeyFile("data/rotationkey.bin", std::ios::binary);
    if (!cc->SerializeEvalAutomorphismKey(rotKeyFile, SerType::BINARY)) {
        std::cerr << "Failed to serialize rotation key" << std::endl;
        return 1;
    }
    rotKeyFile.close();

    // ============================================
    // CREATE AND SERIALIZE INPUT CIPHERTEXT
    // ============================================
    
    std::vector<double> vec = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    
    // Encode as plaintext
    Plaintext ptxt = cc->MakeCKKSPackedPlaintext(vec);
    
    std::cout << "Input: " << ptxt << std::endl;

    // Encrypt
    auto cipher = cc->Encrypt(keyPair.publicKey, ptxt);

    // Serialize input ciphertext
    std::cout << "Serializing input ciphertext..." << std::endl;
    
    if (!Serial::SerializeToFile("data/cipher.bin", cipher, SerType::BINARY)) {
        std::cerr << "Failed to serialize ciphertext" << std::endl;
        return 1;
    }
    
    std::cout << "Serialization complete\n" << std::endl;

    // Clear everything from memory to ensure we're loading from disk
    cc->ClearEvalAutomorphismKeys();
    cipher.reset();

    // ============================================
    // PROFILED COMPUTATION
    // ============================================
    
    std::cout << "Starting profiled rotation by " << rotationIndex << "...\n" << std::endl;
    
    // Start DRAM traffic measurement (includes all I/O and computation)
    g_dram_counter.start();
    
    // Load rotation key from disk
    std::ifstream rotKeyIn("data/rotationkey.bin", std::ios::binary);
    if (!cc->DeserializeEvalAutomorphismKey(rotKeyIn, SerType::BINARY)) {
        std::cerr << "Failed to load rotation key" << std::endl;
        return 1;
    }
    rotKeyIn.close();
    
    // Load input ciphertext from disk
    Ciphertext<DCRTPoly> cipherLoaded;
    if (!Serial::DeserializeFromFile("data/cipher.bin", cipherLoaded, SerType::BINARY)) {
        std::cerr << "Failed to load ciphertext" << std::endl;
        return 1;
    }
    
    // Start integer operation counting
    PIN_MARKER_START();
    
    // Perform rotation (includes key switching)
    auto cipherResult = cc->EvalRotate(cipherLoaded, rotationIndex);
    
    // Stop integer operation counting
    PIN_MARKER_END();
    
    // Serialize and save output ciphertext
    if (!Serial::SerializeToFile("data/result.bin", cipherResult, SerType::BINARY)) {
        std::cerr << "Failed to save result ciphertext" << std::endl;
        return 1;
    }
    
    // Stop DRAM traffic measurement
    g_dram_counter.stop();
    
    // Print DRAM results
    g_dram_counter.print_results(true);
    
    // ============================================
    // VERIFICATION: Decrypt and check result
    // ============================================
    
    std::cout << "\nDecrypting result..." << std::endl;
    
    Plaintext result;
    cc->Decrypt(keyPair.secretKey, cipherResult, &result);
    result->SetLength(vec.size());
    
    std::cout.precision(8);
    std::cout << "Result: " << result << std::endl;
    
    // Show expected values (rotation by 1 shifts everything left)
    std::cout << "Expected: (";
    for (size_t i = 0; i < vec.size(); i++) {
        std::cout << vec[(i + rotationIndex) % vec.size()];
        if (i < vec.size() - 1) std::cout << ", ";
    }
    std::cout << ")" << std::endl;
    
    return 0;
}