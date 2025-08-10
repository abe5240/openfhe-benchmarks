// examples/single-hoisted-diagonal-method.cpp - Hoisted rotation diagonal method for matrix-vector multiplication
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
        std::cout << "=== Single-Hoisted Diagonal Method for Matrix-Vector Multiplication ===\n";
        std::cout << "Actual matrix dimension: " << matrixDim << "×" << matrixDim << "\n";
        std::cout << "Number of slots: " << numSlots << "\n";
        std::cout << "Ring dimension: " << params.ringDim << "\n";
        std::cout << "Multiplicative depth: " << params.multDepth << "\n\n";
    }

    // Generate key pair
    auto keyPair = cc->KeyGen();
    
    // ============================================
    // CREATE MATRIX AND VECTOR
    // ============================================
    
    // Create embedded random matrix and input vector
    auto M = make_embedded_random_matrix(matrixDim, numSlots);
    auto inputVec = make_random_input_vector(matrixDim, numSlots);

    // ============================================
    // EXTRACT AND ENCODE ALL NON-EMPTY DIAGONALS
    // ============================================
    
    if (!quiet) std::cout << "Extracting diagonals...\n";
    
    // Extract all non-empty diagonals
    auto diagonals = extract_generalized_diagonals(M, matrixDim);
    if (!quiet) std::cout << "Found " << diagonals.size() << " non-empty diagonals\n";
    
    // Build parallel vectors for efficient access during computation
    // rotationIndexList[i] corresponds to diagonalPlaintextList[i]
    std::vector<int32_t> rotationIndexList;
    std::vector<Plaintext> diagonalPlaintextList;
    
    rotationIndexList.reserve(diagonals.size());
    diagonalPlaintextList.reserve(diagonals.size());
    
    // Also track which rotations we actually need (skip k=0)
    std::vector<int32_t> rotationsNeeded;
    
    for (const auto& entry : diagonals) {
        int k = entry.first;
        const auto& diag = entry.second;
        
        rotationIndexList.push_back(k);
        diagonalPlaintextList.push_back(cc->MakeCKKSPackedPlaintext(diag));
        
        if (k != 0) {
            rotationsNeeded.push_back(k);
        }
    }
    
    // Create temporary directory for files
    TempDirectory tempDir;
    if (!tempDir.isValid()) {
        std::cerr << "Failed to create temporary directory" << std::endl;
        return 1;
    }
    
    // Generate and save rotation keys individually
    if (!quiet) {
        std::cout << "Generating and saving " << rotationsNeeded.size() << " rotation keys individually...\n";
    }
    
    for (int32_t k : rotationsNeeded) {
        // Generate key for this specific rotation
        cc->EvalRotateKeyGen(keyPair.secretKey, {k});
        
        // Save with descriptive filename
        std::stringstream filename;
        filename << "rotation-key-k" << k << ".bin";
        std::string keyPath = tempDir.getFilePath(filename.str());
        
        std::ofstream keyFile(keyPath, std::ios::binary);
        if (!cc->SerializeEvalAutomorphismKey(keyFile, SerType::BINARY)) {
            std::cerr << "Failed to serialize rotation key for k=" << k << "\n";
            return 1;
        }
        keyFile.close();
        
        // Clear the key from memory immediately after saving
        cc->ClearEvalAutomorphismKeys();
    }
    
    if (!quiet) {
        std::cout << "Saved " << rotationsNeeded.size() << " rotation key files\n";
    }
    
    // Encrypt input vector
    Plaintext inputPtxt = cc->MakeCKKSPackedPlaintext(inputVec);
    auto inputCipher = cc->Encrypt(keyPair.publicKey, inputPtxt);

    // ============================================
    // SERIALIZE INPUT
    // ============================================
    
    if (!quiet) std::cout << "Serializing input...\n";
    
    std::string inputPath = tempDir.getFilePath("input.bin");
    if (!Serial::SerializeToFile(inputPath, inputCipher, SerType::BINARY)) {
        std::cerr << "Failed to serialize input\n";
        return 1;
    }
    
    inputCipher.reset();

    // ============================================
    // PROFILED HOISTED DIAGONAL METHOD COMPUTATION
    // ============================================
    
    if (!quiet) {
        std::cout << "\nStarting profiled computation...\n";
        std::cout << "Will load rotation keys on-demand during computation...\n\n";
    }
    
    // Start DRAM measurement
    measurement.startDRAM();
    
    // Load input
    Ciphertext<DCRTPoly> cipherInput;
    if (!Serial::DeserializeFromFile(inputPath, cipherInput, SerType::BINARY)) {
        std::cerr << "Failed to load input\n";
        return 1;
    }
    
    // PIN markers around the computation
    measurement.startPIN();
    
    // === SINGLE-HOISTED DIAGONAL METHOD WITH ON-DEMAND KEY LOADING ===
    // Step 1: Precompute rotation digits once (hoisting optimization)
    // This allows us to reuse the expensive digit decomposition across all rotations
    if (!quiet) std::cout << "Precomputing rotation digits for hoisting...\n";
    auto precomputedDigits = cc->EvalFastRotationPrecompute(cipherInput);
    
    // Step 2: Get cyclotomic order (needed by EvalFastRotation)
    uint32_t cyclotomicOrder = 2 * cc->GetRingDimension();
    
    // Step 3: Compute result = sum_k diag_k * rotate(input, k)
    // Using hoisted rotations with on-demand key loading
    Ciphertext<DCRTPoly> result;
    bool first = true;
    
    for (std::size_t idx = 0; idx < rotationIndexList.size(); ++idx) {
        int32_t k = rotationIndexList[idx];
        const Plaintext& diagonalPtxt = diagonalPlaintextList[idx];
        
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
                std::cerr << "Failed to load rotation key for k=" << k << "\n";
                return 1;
            }
            keyFile.close();
            
            // Use fast rotation with precomputed digits and loaded key
            rotated = cc->EvalFastRotation(cipherInput, k, cyclotomicOrder, precomputedDigits);
            
            // Clear the key from memory after use
            cc->ClearEvalAutomorphismKeys();
        }
        
        // Multiply by k-th diagonal
        auto partial = cc->EvalMult(rotated, diagonalPtxt);
        
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
        std::cerr << "Failed to save result\n";
        return 1;
    }
    
    // Stop DRAM measurement
    measurement.stopDRAM();
    
    // Print measurement results
    measurement.printResults();
    
    // ============================================
    // VERIFICATION
    // ============================================
    
    if (!skipVerify) {
        if (!quiet) std::cout << "\nDecrypting and verifying result...\n";
        
        Plaintext resultPtxt;
        cc->Decrypt(keyPair.secretKey, result, &resultPtxt);
        resultPtxt->SetLength(numSlots);
        
        auto resultVec = resultPtxt->GetRealPackedValue();
        verify_matrix_vector_result(resultVec, M, inputVec, matrixDim, quiet);
    }

    return 0;
}