// examples/baby-step-giant-step-method.cpp - BSGS diagonal method for matrix-vector multiplication
#include <openfhe.h>
#include <dram_counter.hpp>
#include "utils.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <fstream>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <cmath>

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

// ============================================
// HELPER FUNCTIONS FOR SIGNED INDEXING
// ============================================

// Convert diagonal index from [0, slots-1] to signed range [-slots/2, slots/2]
// This treats the second half of slots as negative indices
// Example: in 64 slots, index 63 becomes -1 (one step backwards)
int normalizeToSignedIndex(int k, int numSlots) {
    int halfSlots = numSlots / 2;
    if (k <= halfSlots) {
        return k;  // First half stays positive
    } else {
        return k - numSlots;  // Second half becomes negative
    }
}

// Floor division that works correctly for negative numbers
// Regular C++ division truncates toward zero, but we need true floor division
// Example: -5 / 3 = -2 (floor), not -1 (truncation)
int floorDivision(int a, int b) {
    int quotient = a / b;
    int remainder = a % b;
    // Adjust if remainder is nonzero and signs differ
    if (remainder != 0 && ((a < 0) != (b < 0))) {
        quotient--;
    }
    return quotient;
}

int main() {
    // Initialize DRAM counter
    if (!g_dram_counter.init()) {
        std::cerr << "Warning: DRAM measurements disabled\n";
    }

    // ============================================
    // SETUP: Configure CKKS parameters
    // ============================================
    
    uint32_t numLimbs     = 4;
    uint32_t numDigits    = 1;
    uint32_t scaleModSize = 50;
    uint32_t ringDim      = 2048; 
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

    int numSlots = static_cast<int>(cc->GetEncodingParams()->GetBatchSize());
    if (static_cast<int>(matrixDim) > numSlots) {
        std::cerr << "Error: matrixDim must be <= numSlots\n";
        return 1;
    }
    
    std::cout << "=== Baby-Step/Giant-Step (BSGS) Method with Signed Indexing ===\n";
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
    print_matrix(M, matrixDim);
    
    // Create random input vector
    auto inputVec = make_random_input_vector(matrixDim, numSlots);

    // ============================================
    // EXTRACT DIAGONALS AND CONVERT TO SIGNED INDEXING
    // ============================================
    
    std::cout << "Extracting diagonals...\n";
    
    // First extract diagonals with regular indexing [0, numSlots-1]
    auto diagonalsUnsigned = extract_generalized_diagonals(M, matrixDim);
    
    // Convert to signed indexing [-numSlots/2, numSlots/2]
    // This allows us to treat the slot ring symmetrically
    std::map<int, std::vector<double>> diagonalsSigned;
    
    for (const auto& entry : diagonalsUnsigned) {
        int kUnsigned = entry.first;
        int kSigned = normalizeToSignedIndex(kUnsigned, numSlots);
        diagonalsSigned[kSigned] = entry.second;
    }
    
    int numDiagonals = static_cast<int>(diagonalsSigned.size());
    std::cout << "Found " << numDiagonals << " non-empty diagonals\n";
    std::cout << "Diagonal indices range from " << diagonalsSigned.begin()->first 
              << " to " << diagonalsSigned.rbegin()->first << "\n";
    
    // ============================================
    // BSGS PARAMETERS BASED ON ACTUAL DIAGONAL COUNT
    // ============================================
    
    // Choose n1 based on the actual number of diagonals (not numSlots!)
    // This is much more efficient for sparse matrices
    int n1 = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(numDiagonals))));
    
    // Ensure n1 is reasonable
    if (n1 < 1) n1 = 1;
    if (n1 > numSlots) n1 = numSlots;
    
    // n2 is informational - we don't actually need it as a fixed bound
    int n2_approx = static_cast<int>(std::ceil(static_cast<double>(numSlots) / n1));
    
    std::cout << "BSGS parameters: n1 = " << n1 
              << " (based on sqrt(" << numDiagonals << ")), n2 ≈ " << n2_approx << "\n";
    
    // ============================================
    // DECOMPOSE DIAGONALS AND PRE-SHIFT
    // ============================================
    
    std::cout << "Pre-shifting diagonals for BSGS decomposition...\n";
    
    // Track which baby steps (i) and giant steps (j) we actually use
    std::set<int> usedBabySteps;   // i ∈ [0, n1)
    std::set<int> usedGiantSteps;  // j can be negative!
    
    // Pre-shift each diagonal by its giant step amount
    std::map<int, Plaintext> preshiftedDiagonals;
    
    for (const auto& entry : diagonalsSigned) {
        int k = entry.first;  // Signed diagonal index
        
        // Decompose k = j*n1 + i where i ∈ [0, n1)
        // Use floor division to handle negative k correctly
        int j = floorDivision(k, n1);
        int i = k - j * n1;  // This ensures i ∈ [0, n1) even for negative k
        
        usedBabySteps.insert(i);
        usedGiantSteps.insert(j);
        
        // Pre-shift the diagonal by j*n1 positions
        auto diagonal = entry.second;
        int shiftAmount = (n1 * j) % numSlots;
        if (shiftAmount < 0) shiftAmount += numSlots;  // Ensure positive shift
        diagonal = rotateVectorDown(diagonal, shiftAmount);
        
        // Store with original signed key k
        preshiftedDiagonals[k] = cc->MakeCKKSPackedPlaintext(diagonal);
    }
    
    std::cout << "Baby steps used: " << usedBabySteps.size() 
              << ", Giant steps used: " << usedGiantSteps.size() << "\n";
    std::cout << "Giant step range: [" << *usedGiantSteps.begin() 
              << ", " << *usedGiantSteps.rbegin() << "]\n";
    
    // ============================================
    // GENERATE AND SAVE ROTATION KEYS
    // ============================================
    
    std::cout << "Generating rotation keys...\n";
    
    // Collect all needed rotations
    std::set<int> rotationIndices;
    
    // Baby rotations (skip i=0 which is identity)
    for (int i : usedBabySteps) {
        if (i != 0) rotationIndices.insert(i);
    }
    
    // Giant rotations (can be negative!)
    for (int j : usedGiantSteps) {
        if (j != 0) {
            int rotation = n1 * j;
            rotationIndices.insert(rotation);
        }
    }
    
    // Generate and save each rotation key
    for (int rot : rotationIndices) {
        cc->EvalRotateKeyGen(keyPair.secretKey, {rot});
        
        std::stringstream filename;
        filename << "data/bsgs-rot-key-" << rot << ".bin";
        
        std::ofstream keyFile(filename.str(), std::ios::binary);
        if (!cc->SerializeEvalAutomorphismKey(keyFile, SerType::BINARY)) {
            std::cerr << "Failed to save rotation key " << rot << "\n";
            return 1;
        }
        keyFile.close();
        cc->ClearEvalAutomorphismKeys();
    }
    
    std::cout << "Generated and saved " << rotationIndices.size() << " rotation keys\n";
    
    // ============================================
    // ENCRYPT INPUT
    // ============================================
    
    std::cout << "Encrypting input...\n";
    
    Plaintext inputPtxt = cc->MakeCKKSPackedPlaintext(inputVec);
    auto inputCipher = cc->Encrypt(keyPair.publicKey, inputPtxt);
    
    if (!Serial::SerializeToFile("data/input.bin", inputCipher, SerType::BINARY)) {
        std::cerr << "Failed to serialize input\n";
        return 1;
    }
    
    inputCipher.reset();

    // ============================================
    // PROFILED BSGS COMPUTATION
    // ============================================
    
    std::cout << "\nStarting profiled BSGS computation...\n";
    
    g_dram_counter.start();
    
    // Load input
    Ciphertext<DCRTPoly> cipherInput;
    if (!Serial::DeserializeFromFile("data/input.bin", cipherInput, SerType::BINARY)) {
        std::cerr << "Failed to load input\n";
        return 1;
    }
    
    PIN_MARKER_START();
    
    // === BSGS COMPUTATION WITH CACHED BABY ROTATIONS ===
    
    // Cache for baby rotations (compute on first use)
    std::vector<Ciphertext<DCRTPoly>> babyRotationCache(n1);
    std::vector<bool> babyRotationComputed(n1, false);
    
    // Identity rotation is always available
    babyRotationCache[0] = cipherInput;
    babyRotationComputed[0] = true;
    
    // Helper to get/compute baby rotation
    auto getBabyRotation = [&](int i) -> const Ciphertext<DCRTPoly>& {
        if (!babyRotationComputed[i]) {
            // Load rotation key
            std::stringstream filename;
            filename << "data/bsgs-rot-key-" << i << ".bin";
            
            std::ifstream keyFile(filename.str(), std::ios::binary);
            if (!cc->DeserializeEvalAutomorphismKey(keyFile, SerType::BINARY)) {
                std::cerr << "Failed to load key for baby step " << i << "\n";
                throw std::runtime_error("Missing rotation key");
            }
            keyFile.close();
            
            // Compute and cache rotation
            babyRotationCache[i] = cc->EvalRotate(cipherInput, i);
            cc->ClearEvalAutomorphismKeys();
            babyRotationComputed[i] = true;
        }
        return babyRotationCache[i];
    };
    
    // Process giant steps in sorted order (from negative to positive)
    // This ensures consistent numerical behavior
    std::vector<int> sortedGiantSteps(usedGiantSteps.begin(), usedGiantSteps.end());
    std::sort(sortedGiantSteps.begin(), sortedGiantSteps.end());
    
    Ciphertext<DCRTPoly> result;
    bool first = true;
    
    for (int j : sortedGiantSteps) {
        
        // Accumulate all baby steps for this giant block
        Ciphertext<DCRTPoly> giantBlockSum;
        bool giantBlockFirst = true;
        
        // Check all possible baby steps for this giant block
        for (int i = 0; i < n1; ++i) {
            // Reconstruct the signed diagonal index
            int k = j * n1 + i;
            
            // Check if this diagonal exists
            auto diagIter = preshiftedDiagonals.find(k);
            if (diagIter == preshiftedDiagonals.end()) continue;
            
            // Get baby rotation (from cache or compute)
            const auto& babyRotated = (i == 0) ? cipherInput : getBabyRotation(i);
            
            // Multiply with pre-shifted diagonal
            auto partial = cc->EvalMult(babyRotated, diagIter->second);
            
            // Accumulate within giant block
            if (giantBlockFirst) {
                giantBlockSum = partial;
                giantBlockFirst = false;
            } else {
                giantBlockSum = cc->EvalAdd(giantBlockSum, partial);
            }
        }
        
        // Skip if block is empty
        if (giantBlockFirst) continue;
        
        // Apply giant rotation if j ≠ 0
        if (j != 0) {
            int giantRotation = n1 * j;  // Can be negative!
            
            // Load rotation key
            std::stringstream filename;
            filename << "data/bsgs-rot-key-" << giantRotation << ".bin";
            
            std::ifstream keyFile(filename.str(), std::ios::binary);
            if (!cc->DeserializeEvalAutomorphismKey(keyFile, SerType::BINARY)) {
                std::cerr << "Failed to load key for giant step " << giantRotation << "\n";
                return 1;
            }
            keyFile.close();
            
            giantBlockSum = cc->EvalRotate(giantBlockSum, giantRotation);
            cc->ClearEvalAutomorphismKeys();
        }
        
        // Add to result
        if (first) {
            result = giantBlockSum;
            first = false;
        } else {
            result = cc->EvalAdd(result, giantBlockSum);
        }
    }
    
    // Final rescale to manage noise
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