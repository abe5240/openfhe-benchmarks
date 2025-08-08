// examples/single-hoisted-diagonal-method.cpp - Hoisted rotation diagonal method for matrix-vector multiplication
#include <openfhe.h>
#include <dram_counter.hpp>
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
    
    uint32_t numLimbs     = 10;
    uint32_t numDigits    = 2;
    uint32_t scaleModSize = 50;
    uint32_t ringDim      = 128; 
    std::size_t matrixDim = 16;  // Actual matrix dimension

    CCParams<CryptoContextCKKSRNS> params;
    params.SetMultiplicativeDepth(numLimbs - 1);
    params.SetScalingModSize(scaleModSize);
    params.SetRingDim(ringDim);
    params.SetScalingTechnique(FIXEDMANUAL);
    params.SetKeySwitchTechnique(HYBRID);
    params.SetNumLargeDigits(numDigits);
    params.SetSecurityLevel(HEStd_NotSet);

    // Create cryptocontext
    CryptoContext<DCRTPoly> cc = GenCryptoContext(params);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    uint32_t numSlots = cc->GetEncodingParams()->GetBatchSize();
    if (matrixDim > numSlots) {
        std::cerr << "Error: matrixDim must be <= numSlots\n";
        return 1;
    }
    
    std::cout << "=== Single-Hoisted Diagonal Method for Matrix-Vector Multiplication ===\n";
    std::cout << "Actual matrix dimension: " << matrixDim << "×" << matrixDim << "\n";
    std::cout << "Number of slots: " << numSlots << "\n";
    std::cout << "Ring dimension: " << ringDim << "\n\n";

    // Generate key pair
    auto keyPair = cc->KeyGen();
    
    // ============================================
    // CREATE MATRIX AND VECTOR
    // ============================================
    
    // Create embedded random matrix
    auto M = make_embedded_random_matrix(matrixDim, numSlots);
    auto inputVec = make_random_input_vector(matrixDim, numSlots);
    print_matrix(M, matrixDim);

    // ============================================
    // EXTRACT AND ENCODE ALL NON-EMPTY DIAGONALS
    // ============================================
    
    std::cout << "Extracting diagonals...\n";
    
    // Extract all non-empty diagonals
    auto diagonals = extract_generalized_diagonals(M, matrixDim);
    std::cout << "Found " << diagonals.size() << " non-empty diagonals\n";
    
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
    
    // Generate and save rotation keys individually
    std::cout << "Generating and saving " << rotationsNeeded.size() << " rotation keys individually...\n";
    
    for (int32_t k : rotationsNeeded) {
        // Generate key for this specific rotation
        cc->EvalRotateKeyGen(keyPair.secretKey, {k});
        
        // Save with descriptive filename: rotation-key-k{value}.bin
        std::stringstream filename;
        filename << "data/rotation-key-k" << k << ".bin";
        
        std::ofstream keyFile(filename.str(), std::ios::binary);
        if (!cc->SerializeEvalAutomorphismKey(keyFile, SerType::BINARY)) {
            std::cerr << "Failed to serialize rotation key for k=" << k << "\n";
            return 1;
        }
        keyFile.close();
        
        // Clear the key from memory immediately after saving
        cc->ClearEvalAutomorphismKeys();
    }
    
    std::cout << "Saved " << rotationsNeeded.size() << " rotation key files\n";
    
    // Encrypt input vector
    Plaintext inputPtxt = cc->MakeCKKSPackedPlaintext(inputVec);
    auto inputCipher = cc->Encrypt(keyPair.publicKey, inputPtxt);

    // ============================================
    // SERIALIZE INPUT
    // ============================================
    
    std::cout << "Serializing input...\n";
    
    if (!Serial::SerializeToFile("data/input.bin", inputCipher, SerType::BINARY)) {
        std::cerr << "Failed to serialize input\n";
        return 1;
    }
    
    inputCipher.reset();

    // ============================================
    // PROFILED HOISTED DIAGONAL METHOD COMPUTATION
    // ============================================
    
    std::cout << "\nStarting profiled computation...\n";
    std::cout << "Will load rotation keys on-demand during computation...\n\n";
    
    g_dram_counter.start();
    
    // Load input
    Ciphertext<DCRTPoly> cipherInput;
    if (!Serial::DeserializeFromFile("data/input.bin", cipherInput, SerType::BINARY)) {
        std::cerr << "Failed to load input\n";
        return 1;
    }
    
    PIN_MARKER_START();
    
    // === SINGLE-HOISTED DIAGONAL METHOD WITH ON-DEMAND KEY LOADING ===
    // Step 1: Precompute rotation digits once (hoisting optimization)
    // This allows us to reuse the expensive digit decomposition across all rotations
    std::cout << "Precomputing rotation digits for hoisting...\n";
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
            filename << "data/rotation-key-k" << k << ".bin";
            
            std::ifstream keyFile(filename.str(), std::ios::binary);
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
    
    // Rescale once at the end to manage noise
    result = cc->Rescale(result);
    
    PIN_MARKER_END();
    
    // Save result
    if (!Serial::SerializeToFile("data/result.bin", result, SerType::BINARY)) {
        std::cerr << "Failed to save result\n";
        return 1;
    }
    
    g_dram_counter.stop();
    g_dram_counter.print_results(true);
    
    // ============================================
    // VERIFICATION
    // ============================================
    
    std::cout << "\nDecrypting and verifying result...\n";
    
    Plaintext resultPtxt;
    cc->Decrypt(keyPair.secretKey, result, &resultPtxt);
    resultPtxt->SetLength(numSlots);
    
    auto resultVec = resultPtxt->GetRealPackedValue();
    verify_matrix_vector_result(resultVec, M, inputVec, matrixDim);

    return 0;
}