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
    // Parse arguments
    ArgParser parser;
    parser.parse(argc, argv);
    
    // Get parameters
    bool quiet = parser.getBool("quiet", false);
    bool skipVerify = parser.getBool("skip-verify", false);
    std::size_t matrixDim = static_cast<std::size_t>(parser.getUInt32("matrix-dim", 128));
    setupThreads(parser);
    
    MeasurementMode mode = parser.getMeasurementMode();
    MeasurementSystem measurement(mode);
    
    BenchmarkParams params = BenchmarkParams::fromArgs(parser);
        
    // Setup CKKS cryptocontext
    CCParams<CryptoContextCKKSRNS> ccParams;
    ccParams.SetMultiplicativeDepth(params.multDepth);
    ccParams.SetScalingModSize(50);
    ccParams.SetRingDim(params.ringDim);
    ccParams.SetScalingTechnique(FLEXIBLEAUTO);
    ccParams.SetKeySwitchTechnique(HYBRID);
    ccParams.SetNumLargeDigits(params.numDigits);
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
        std::cout << "=== Diagonal Method for Matrix-Vector Multiplication ===\n";
        std::cout << "Matrix dimension: " << matrixDim << "x" << matrixDim << "\n";
        std::cout << "Number of slots: " << numSlots << "\n";
        std::cout << "Ring dimension: " << params.ringDim << "\n\n";
    }

    // Generate key pair
    auto keyPair = cc->KeyGen();
    
    // Create embedded random matrix and input vector
    auto M = make_embedded_random_matrix(matrixDim, numSlots);
    auto inputVec = make_random_input_vector(matrixDim, numSlots);

    // Extract all non-empty diagonals
    auto diagonals = extract_generalized_diagonals(M, matrixDim);
    
    if (!quiet) {
        std::cout << "Found " << diagonals.size() << " non-empty diagonals\n";
    }
    
    // Create temporary directory for files
    TempDirectory tempDir;
    if (!tempDir.isValid()) {
        std::cerr << "Failed to create temporary directory" << std::endl;
        return 1;
    }
    
    // Generate and serialize rotation keys individually
    std::vector<int32_t> rotationIndices;
    for (const auto& entry : diagonals) {
        int k = entry.first;
        if (k != 0) {
            rotationIndices.push_back(k);
        }
    }
    
    // Generate and save each rotation key separately
    for (int32_t k : rotationIndices) {
        // Generate key for this specific rotation
        cc->EvalRotateKeyGen(keyPair.secretKey, {k});
        
        // Save with descriptive filename
        std::stringstream filename;
        filename << "rotation-key-k" << k << ".bin";
        std::string keyPath = tempDir.getFilePath(filename.str());
        
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

    // Serialize input
    std::string inputPath = tempDir.getFilePath("input.bin");
    if (!Serial::SerializeToFile(inputPath, inputCipher, SerType::BINARY)) {
        std::cerr << "Failed to serialize input" << std::endl;
        return 1;
    }
    
    inputCipher.reset();

    // PROFILED DIAGONAL METHOD COMPUTATION
    
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
    
    // Diagonal method: result = sum_k diag_k * rotate(input, k)
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
            std::string keyPath = tempDir.getFilePath(filename.str());
            
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
    std::string resultPath = tempDir.getFilePath("result.bin");
    if (!Serial::SerializeToFile(resultPath, result, SerType::BINARY)) {
        std::cerr << "Failed to save result" << std::endl;
        return 1;
    }
    
    // Stop DRAM measurement
    measurement.stopDRAM();
    
    // Print measurement results
    measurement.printResults();
    
    // Verification (optional)
    if (!skipVerify) {
        Plaintext resultPtxt;
        cc->Decrypt(keyPair.secretKey, result, &resultPtxt);
        resultPtxt->SetLength(numSlots);
        
        auto resultVec = resultPtxt->GetRealPackedValue();
        verify_matrix_vector_result(resultVec, M, inputVec, matrixDim, quiet);
    }
    
    return 0;
}