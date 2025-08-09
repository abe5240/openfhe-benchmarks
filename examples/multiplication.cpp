// examples/multiplication.cpp - Minimal benchmark for homomorphic multiplication
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
        parser.printUsage("multiplication");
        return 0;
    }
    
    // Get common parameters
    bool quiet = parser.getBool("quiet", false);
    bool skipVerify = parser.getBool("skip-verify", false);
    
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
    
    // For multiplication, we typically need these techniques
    ccParams.SetScalingTechnique(FLEXIBLEAUTO);
    ccParams.SetKeySwitchTechnique(HYBRID);
    ccParams.SetNumLargeDigits(2);  // Good default for multiplication
    
    CryptoContext<DCRTPoly> cc = GenCryptoContext(ccParams);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    if (!quiet) {
        std::cout << "=== Multiplication Benchmark ===" << std::endl;
    }

    // Generate key pair
    auto keyPair = cc->KeyGen();
    
    // Generate multiplication key (needed for EvalMult)
    cc->EvalMultKeyGen(keyPair.secretKey);

    // ============================================
    // PREPARE TEST DATA
    // ============================================
    
    std::vector<double> vec1 = {1.0, 1.1, 1.2, 1.3, 1.4, 1.5, 1.6, 1.7};
    std::vector<double> vec2 = {2.0, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7};
    
    // Encode as plaintexts
    Plaintext ptxt1 = cc->MakeCKKSPackedPlaintext(vec1);
    Plaintext ptxt2 = cc->MakeCKKSPackedPlaintext(vec2);
    
    if (!quiet) {
        std::cout << "Input 1: " << ptxt1 << std::endl;
        std::cout << "Input 2: " << ptxt2 << std::endl;
    }

    // Encrypt
    auto cipher1 = cc->Encrypt(keyPair.publicKey, ptxt1);
    auto cipher2 = cc->Encrypt(keyPair.publicKey, ptxt2);

    // ============================================
    // SERIALIZE TO TEMPORARY FILES
    // ============================================
    
    TempDirectory tempDir;
    if (!tempDir.isValid()) {
        std::cerr << "Failed to create temporary directory" << std::endl;
        return 1;
    }

    if (!quiet) {
        std::cout << "Serializing input ciphertexts and multiplication key..." << std::endl;
    }
    
    std::string cipher1Path = tempDir.getFilePath("cipher1.bin");
    std::string cipher2Path = tempDir.getFilePath("cipher2.bin");
    std::string multKeyPath = tempDir.getFilePath("evalmultkey.bin");
    std::string resultPath = tempDir.getFilePath("result.bin");
    
    // Serialize input ciphertexts
    if (!Serial::SerializeToFile(cipher1Path, cipher1, SerType::BINARY)) {
        std::cerr << "Failed to serialize ciphertext 1" << std::endl;
        return 1;
    }
    
    if (!Serial::SerializeToFile(cipher2Path, cipher2, SerType::BINARY)) {
        std::cerr << "Failed to serialize ciphertext 2" << std::endl;
        return 1;
    }
    
    // Serialize multiplication key
    std::ofstream multKeyFile(multKeyPath, std::ios::binary);
    if (!cc->SerializeEvalMultKey(multKeyFile, SerType::BINARY)) {
        std::cerr << "Failed to serialize EvalMult key" << std::endl;
        return 1;
    }
    multKeyFile.close();
    
    if (!quiet) {
        std::cout << "Serialization complete\n" << std::endl;
        std::cout << "Starting profiled multiplication...\n" << std::endl;
    }

    // Clear everything from memory to ensure we're loading from disk
    cc->ClearEvalMultKeys();
    cipher1.reset();
    cipher2.reset();

    // ============================================
    // PROFILED FHE COMPUTATION
    // ============================================
    
    // Start DRAM measurement (includes all I/O and computation)
    measurement.startDRAM();

    // Load multiplication key from disk
    std::ifstream multKeyIn(multKeyPath, std::ios::binary);
    if (!cc->DeserializeEvalMultKey(multKeyIn, SerType::BINARY)) {
        std::cerr << "Failed to load EvalMult key" << std::endl;
        return 1;
    }
    multKeyIn.close();
    
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
    
    // PIN markers around ONLY the FHE operation
    measurement.startPIN();
    
    // Perform homomorphic multiplication (includes relinearization)
    auto cipherResult = cc->EvalMult(c1Loaded, c2Loaded);
    
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
        result->SetLength(vec1.size());
        
        // Get actual result vector
        auto resultVec = result->GetRealPackedValue();
        
        // Compute expected values
        std::vector<double> expected;
        for (size_t i = 0; i < vec1.size(); i++) {
            expected.push_back(vec1[i] * vec2[i]);
        }
        
        // Use the simple verification function from utils.hpp
        verifyResult(resultVec, expected);
    }
    
    return 0;
}