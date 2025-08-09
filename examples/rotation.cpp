// examples/rotation.cpp - Minimal benchmark for homomorphic rotation
#include <openfhe.h>
#include "utils.hpp"
#include <iostream>
#include <vector>

// Headers needed for serialization
#include <ciphertext-ser.h>
#include <cryptocontext-ser.h>
#include <key/key-ser.h>
#include <scheme/ckksrns/ckksrns-ser.h>

using namespace lbcrypto;

int main(int argc, char* argv[]) {
    // ============================================
    // PARSE ARGUMENTS AND SETUP
    // ============================================
    
    ArgParser parser;
    parser.parse(argc, argv);
    
    if (parser.hasHelp()) {
        parser.printUsage("rotation");
        std::cout << "\nAdditional options:\n";
        std::cout << "  --rotation-index=N    Rotation index (default: 1)\n";
        return 0;
    }
    
    // Get common parameters
    bool quiet = parser.getBool("quiet", false);
    bool skipVerify = parser.getBool("skip-verify", false);
    
    // Get rotation-specific parameter
    int32_t rotationIndex = static_cast<int32_t>(parser.getUInt32("rotation-index", 1));
    
    // Initialize thread management
    ThreadManager threads(parser);
    threads.initialize();
    
    // Setup measurement system
    MeasurementMode mode = parser.getMeasurementMode();
    MeasurementSystem measurement(mode, quiet);
    measurement.initialize();
    
    // Get benchmark parameters
    BenchmarkParams params = BenchmarkParams::fromArgs(parser);
    
    if (!quiet) {
        params.print();
        std::cout << "Rotation index: " << rotationIndex << std::endl;
        std::cout << "Measurement mode: " << measurement.getModeString() << "\n\n";
    }
    
    // ============================================
    // SETUP CKKS CRYPTOCONTEXT
    // ============================================
    
    CCParams<CryptoContextCKKSRNS> ccParams;
    ccParams.SetMultiplicativeDepth(params.multDepth);
    ccParams.SetScalingModSize(params.scaleModSize);
    ccParams.SetRingDim(params.ringDim);
    ccParams.SetSecurityLevel(params.checkSecurity ? HEStd_128_classic : HEStd_NotSet);
    
    // For rotation, we typically need these techniques
    ccParams.SetScalingTechnique(FLEXIBLEAUTO);
    ccParams.SetKeySwitchTechnique(HYBRID);
    ccParams.SetNumLargeDigits(2);  // Good default for rotation
    
    CryptoContext<DCRTPoly> cc = GenCryptoContext(ccParams);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    if (!quiet) {
        std::cout << "=== Rotation Benchmark ===" << std::endl;
    }

    // Generate key pair
    auto keyPair = cc->KeyGen();
    
    // Generate rotation keys for the specified index
    std::vector<int32_t> rotationIndices = {rotationIndex};
    cc->EvalRotateKeyGen(keyPair.secretKey, rotationIndices);

    // ============================================
    // PREPARE TEST DATA
    // ============================================
    
    std::vector<double> vec = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    
    // Encode as plaintext
    Plaintext ptxt = cc->MakeCKKSPackedPlaintext(vec);
    
    if (!quiet) {
        std::cout << "Input: " << ptxt << std::endl;
        std::cout << "Rotating by: " << rotationIndex << " positions" << std::endl;
    }

    // Encrypt
    auto cipher = cc->Encrypt(keyPair.publicKey, ptxt);

    // ============================================
    // SERIALIZE TO TEMPORARY FILES
    // ============================================
    
    TempDirectory tempDir;
    if (!tempDir.isValid()) {
        std::cerr << "Failed to create temporary directory" << std::endl;
        return 1;
    }

    if (!quiet) {
        std::cout << "Serializing input ciphertext and rotation key..." << std::endl;
    }
    
    std::string cipherPath = tempDir.getFilePath("cipher.bin");
    std::string rotKeyPath = tempDir.getFilePath("rotationkey.bin");
    std::string resultPath = tempDir.getFilePath("result.bin");
    
    // Serialize input ciphertext
    if (!Serial::SerializeToFile(cipherPath, cipher, SerType::BINARY)) {
        std::cerr << "Failed to serialize ciphertext" << std::endl;
        return 1;
    }
    
    // Serialize rotation key
    std::ofstream rotKeyFile(rotKeyPath, std::ios::binary);
    if (!cc->SerializeEvalAutomorphismKey(rotKeyFile, SerType::BINARY)) {
        std::cerr << "Failed to serialize rotation key" << std::endl;
        return 1;
    }
    rotKeyFile.close();
    
    if (!quiet) {
        std::cout << "Serialization complete\n" << std::endl;
        std::cout << "Starting profiled rotation...\n" << std::endl;
    }

    // Clear everything from memory to ensure we're loading from disk
    cc->ClearEvalAutomorphismKeys();
    cipher.reset();

    // ============================================
    // PROFILED FHE COMPUTATION
    // ============================================
    
    // Start DRAM measurement (includes all I/O and computation)
    measurement.startDRAM();

    // Load rotation key from disk
    std::ifstream rotKeyIn(rotKeyPath, std::ios::binary);
    if (!cc->DeserializeEvalAutomorphismKey(rotKeyIn, SerType::BINARY)) {
        std::cerr << "Failed to load rotation key" << std::endl;
        return 1;
    }
    rotKeyIn.close();
    
    // Load input ciphertext from disk
    Ciphertext<DCRTPoly> cipherLoaded;
    
    if (!Serial::DeserializeFromFile(cipherPath, cipherLoaded, SerType::BINARY)) {
        std::cerr << "Failed to load ciphertext" << std::endl;
        return 1;
    }
    
    // PIN markers around ONLY the FHE operation
    measurement.startPIN();
    
    // Perform homomorphic rotation (includes key switching)
    auto cipherResult = cc->EvalRotate(cipherLoaded, rotationIndex);
    
    measurement.endPIN();
        
    // Serialize result
    if (!Serial::SerializeToFile(resultPath, cipherResult, SerType::BINARY)) {
        std::cerr << "Failed to save result ciphertext" << std::endl;
        return 1;
    }
    
    // Stop DRAM measurement
    measurement.stopDRAM();
    
    // Print measurement results
    measurement.printResults();
    
    // ============================================
    // VERIFICATION (OPTIONAL)
    // ============================================
    
    if (!quiet && !skipVerify) {
        std::cout << "\nDecrypting result..." << std::endl;
        
        Plaintext result;
        cc->Decrypt(keyPair.secretKey, cipherResult, &result);
        result->SetLength(vec.size());
        
        // Get actual result vector
        auto resultVec = result->GetRealPackedValue();
        
        // Compute expected values (rotation shifts elements left)
        std::vector<double> expected;
        for (size_t i = 0; i < vec.size(); i++) {
            // Positive rotation index rotates left (cyclically)
            size_t srcIndex = (i + rotationIndex) % vec.size();
            // Handle negative rotation indices correctly
            if (rotationIndex < 0) {
                int adjustedIndex = static_cast<int>(i) + rotationIndex;
                while (adjustedIndex < 0) {
                    adjustedIndex += vec.size();
                }
                srcIndex = adjustedIndex;
            }
            expected.push_back(vec[srcIndex]);
        }
        
        // Use the simple verification function from utils.hpp
        verifyResult(resultVec, expected);
    }
    
    return 0;
}