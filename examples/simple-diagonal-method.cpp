// examples/simple-diagonal-method.cpp - Diagonal method for matrix-vector multiplication
#include <openfhe.h>
#include "utils.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <fstream>
#include <algorithm>
#include <map>
#include <sstream>

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
        parser.printUsage("simple-diagonal-method");
        std::cout << "\nAdditional options:\n";
        std::cout << "  --matrix-dim=N        Matrix dimension (default: 16)\n";
        std::cout << "  --num-limbs=N         Number of limbs for depth (default: 4)\n";
        std::cout << "  --num-digits=N        Number of large digits (default: 1)\n";
        return 0;
    }
    
    // Get common parameters
    bool quiet = parser.getBool("quiet", false);
    bool skipVerify = parser.getBool("skip-verify", false);
    
    // Get method-specific parameters
    std::size_t matrixDim = static_cast<std::size_t>(parser.getUInt32("matrix-dim", 16));
    uint32_t numLimbs = parser.getUInt32("num-limbs", 4);
    uint32_t numDigits = parser.getUInt32("num-digits", 1);
    
    // Initialize thread management
    ThreadManager threads(parser);
    threads.initialize();
    
    // Setup measurement system
    MeasurementMode mode = parser.getMeasurementMode();
    MeasurementSystem measurement(mode, quiet);
    measurement.initialize();
    
    // Get benchmark parameters (override some defaults for this method)
    BenchmarkParams params = BenchmarkParams::fromArgs(parser);
    
    // Override ring dimension if not specified - use smaller default for simple diagonal
    if (parser.getString("ring-dim").empty()) {
        params.ringDim = 128;  // Default for simple diagonal method
    }
    
    // Override mult depth based on numLimbs
    params.multDepth = numLimbs - 1;
    
    if (!quiet) {
        params.print();
        std::cout << "Matrix dimension: " << matrixDim << "×" << matrixDim << std::endl;
        std::cout << "Number of limbs: " << numLimbs << std::endl;
        std::cout << "Number of digits: " << numDigits << std::endl;
        std::cout << "Measurement mode: " << measurement.getModeString() << "\n\n";
    }
    
    // ============================================
    // SETUP CKKS CRYPTOCONTEXT
    // ============================================
    
    CCParams<CryptoContextCKKSRNS> ccParams;
    ccParams.SetMultiplicativeDepth(params.multDepth);
    ccParams.SetScalingModSize(params.scaleModSize);
    ccParams.SetRingDim(params.ringDim);
    ccParams.SetScalingTechnique(FLEXIBLEAUTO);
    ccParams.SetKeySwitchTechnique(HYBRID);
    ccParams.SetNumLargeDigits(numDigits);
    ccParams.SetSecurityLevel(params.checkSecurity ? HEStd_128_classic : HEStd_NotSet);
    
    CryptoContext<DCRTPoly> cc = GenCryptoContext(ccParams);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    uint32_t numSlots = cc->GetEncodingParams()->GetBatchSize();
    if (matrixDim > numSlots) {
        std::cerr << "Error: matrixDim (" << matrixDim << ") must be <= numSlots (" << numSlots << ")\n";
        return 1;
    }
    
    if (!quiet) {
        std::cout << "=== Diagonal Method for Matrix-Vector Multiplication ===" << std::endl;
        std::cout << "Actual matrix dimension: " << matrixDim << "×" << matrixDim << std::endl;
        std::cout << "Number of slots: " << numSlots << std::endl;
    }

    // Generate key pair
    auto keyPair = cc->KeyGen();
    
    // ============================================
    // CREATE MATRIX AND VECTOR
    // ============================================
    
    // Create embedded random matrix and input vector
    auto M = make_embedded_random_matrix(matrixDim, numSlots);
    auto inputVec = make_random_input_vector(matrixDim, numSlots);
    
    if (!quiet) {
        print_matrix(M, matrixDim);
    }

    // ============================================
    // EXTRACT AND ENCODE ALL NON-EMPTY DIAGONALS
    // ============================================
    
    if (!quiet) {
        std::cout << "Extracting diagonals..." << std::endl;
    }
    
    // Extract all non-empty diagonals
    auto diagonals = extract_generalized_diagonals(M, matrixDim);
    
    if (!quiet) {
        std::cout << "Found " << diagonals.size() << " non-empty diagonals" << std::endl;
    }
    
    // Create temporary directory for files
    TempDirectory tempDir;
    if (!tempDir.isValid()) {
        std::cerr << "Failed to create temporary directory" << std::endl;
        return 1;
    }
    
    // Generate and serialize rotation keys individually
    // k=0 doesn't need rotation, so we skip it
    std::vector<int32_t> rotationIndices;
    for (const auto& entry : diagonals) {
        int k = entry.first;
        if (k != 0) {
            rotationIndices.push_back(k);
        }
    }
    
    if (!quiet) {
        std::cout << "Generating and saving " << rotationIndices.size() << " rotation keys..." << std::endl;
    }
    
    // Generate and save each rotation key separately
    for (int32_t k : rotationIndices) {
        // Generate key for this specific rotation
        cc->EvalRotateKeyGen(keyPair.secretKey, {k});
        
        // Save with descriptive filename
        std::stringstream filename;
        filename << "rotation-key-k" << k << ".bin";
        std::string keyPath = tempDir.getDataPath(filename.str());
        
        std::ofstream keyFile(keyPath, std::ios::binary);
        if (!cc->SerializeEvalAutomorphismKey(keyFile, SerType::BINARY)) {
            std::cerr << "Failed to serialize rotation key for k=" << k << std::endl;
            return 1;
        }
        keyFile.close();
        
        // Clear the key from memory immediately after saving
        cc->ClearEvalAutomorphismKeys();
    }
    
    // Encode diagonals as plaintexts
    std::map<int, Plaintext> diagonalPlaintexts;
    for (const auto& entry : diagonals) {
        int k = entry.first;
        const auto& diag = entry.second;
        diagonalPlaintexts[k] = cc->MakeCKKSPackedPlaintext(diag);
    }
    
    // Encrypt input vector
    Plaintext inputPtxt = cc->MakeCKKSPackedPlaintext(inputVec);
    auto inputCipher = cc->Encrypt(keyPair.publicKey, inputPtxt);

    // ============================================
    // SERIALIZE INPUT
    // ============================================
    
    if (!quiet) {
        std::cout << "Serializing input..." << std::endl;
    }
    
    std::string inputPath = tempDir.getDataPath("input.bin");
    if (!Serial::SerializeToFile(inputPath, inputCipher, SerType::BINARY)) {
        std::cerr << "Failed to serialize input" << std::endl;
        return 1;
    }
    
    inputCipher.reset();

    if (!quiet) {
        std::cout << "Starting profiled computation...\n" << std::endl;
    }

    // ============================================
    // PROFILED DIAGONAL METHOD COMPUTATION
    // ============================================
    
    // Start DRAM measurement
    measurement.startDRAM();
    
    // Load input
    Ciphertext<DCRTPoly> cipherInput;
    if (!Serial::DeserializeFromFile(inputPath, cipherInput, SerType::BINARY)) {
        std::cerr << "Failed to load input" << std::endl;
        return 1;
    }
    
    // PIN markers around the computation
    measurement.startPIN();
    
    // === SIMPLIFIED DIAGONAL METHOD WITH ON-DEMAND KEY LOADING ===
    // Compute: result = sum_k diag_k * rotate(input, k)
    // Loading each rotation key only when needed
    
    Ciphertext<DCRTPoly> result;
    bool first = true;

    // Process all non-empty diagonals
    for (const auto& entry : diagonals) {
        int k = entry.first;
        
        Ciphertext<DCRTPoly> rotated;
        
        if (k == 0) {
            // No rotation needed for main diagonal
            rotated = cipherInput;
        } else {
            // Load the specific rotation key for this k value
            std::stringstream filename;
            filename << "rotation-key-k" << k << ".bin";
            std::string keyPath = tempDir.getDataPath(filename.str());
            
            std::ifstream keyFile(keyPath, std::ios::binary);
            if (!cc->DeserializeEvalAutomorphismKey(keyFile, SerType::BINARY)) {
                std::cerr << "Failed to load rotation key for k=" << k << std::endl;
                return 1;
            }
            keyFile.close();
            
            // Perform rotation with the loaded key
            rotated = cc->EvalRotate(cipherInput, k);
            
            // Clear the key from memory after use
            cc->ClearEvalAutomorphismKeys();
        }
        
        // Multiply by k-th diagonal
        auto partial = cc->EvalMult(rotated, diagonalPlaintexts[k]);
        
        // Accumulate
        if (first) {
            result = partial;
            first = false;
        } else {
            result = cc->EvalAdd(result, partial);
        }
    }
    
    measurement.endPIN();
    
    // Save result
    std::string resultPath = tempDir.getDataPath("result.bin");
    if (!Serial::SerializeToFile(resultPath, result, SerType::BINARY)) {
        std::cerr << "Failed to save result" << std::endl;
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
        std::cout << "\nDecrypting and verifying result..." << std::endl;
        
        Plaintext resultPtxt;
        cc->Decrypt(keyPair.secretKey, result, &resultPtxt);
        resultPtxt->SetLength(numSlots);
        
        auto resultVec = resultPtxt->GetRealPackedValue();
        verify_matrix_vector_result(resultVec, M, inputVec, matrixDim);
    }
    
    return 0;
}