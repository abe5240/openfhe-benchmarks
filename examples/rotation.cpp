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
    // Parse arguments
    ArgParser parser;
    parser.parse(argc, argv);
    
    // Get parameters
    bool debug = parser.getDebug();
    int32_t rotationIndex = static_cast<int32_t>(parser.getUInt32("rotation-index", 1));
    setupThreads(parser);
    
    MeasurementMode mode = parser.getMeasurementMode();
    MeasurementSystem measurement(mode);
    
    BenchmarkParams params = BenchmarkParams::fromArgs(parser);
    
    // Setup CKKS cryptocontext
    CCParams<CryptoContextCKKSRNS> ccParams;
    ccParams.SetMultiplicativeDepth(params.multDepth);
    ccParams.SetScalingModSize(50);
    ccParams.SetRingDim(params.ringDim);
    ccParams.SetSecurityLevel(params.checkSecurity ? HEStd_128_classic : HEStd_NotSet);
    
    ccParams.SetScalingTechnique(FLEXIBLEAUTO);
    ccParams.SetKeySwitchTechnique(HYBRID);
    ccParams.SetNumLargeDigits(params.numDigits);
    
    CryptoContext<DCRTPoly> cc = GenCryptoContext(ccParams);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    // Generate key pair
    auto keyPair = cc->KeyGen();
    
    // Generate rotation keys for the specified index
    std::vector<int32_t> rotationIndices = {rotationIndex};
    cc->EvalRotateKeyGen(keyPair.secretKey, rotationIndices);

    // Get number of slots
    uint32_t numSlots = cc->GetEncodingParams()->GetBatchSize();
    
    // Prepare test data 
    std::vector<double> vec = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
    vec.resize(numSlots, 0.0); 
    
    // Encode as plaintext
    Plaintext ptxt = cc->MakeCKKSPackedPlaintext(vec);

    // Encrypt
    auto cipher = cc->Encrypt(keyPair.publicKey, ptxt);

    // Serialize to temporary files
    TempDirectory tempDir;
    if (!tempDir.isValid()) {
        std::cerr << "Failed to create temporary directory" << std::endl;
        return 1;
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

    // Clear everything from memory to ensure we're loading from disk
    cc->ClearEvalAutomorphismKeys();
    cipher.reset();

    // PROFILED FHE COMPUTATION
    
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
    
    // Always verify
    Plaintext result;
    cc->Decrypt(keyPair.secretKey, cipherResult, &result);
    result->SetLength(numSlots);
    
    // Get actual result vector
    auto resultVec = result->GetRealPackedValue();
    
    // Use rotate function that matches OpenFHE's direction
    auto expected = rotate(vec, rotationIndex);
    
    // Verify and return exit code
    return verifyResult(resultVec, expected, debug) ? 0 : 1;
}