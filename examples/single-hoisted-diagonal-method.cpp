// examples/single-hoisted-diagonal-method.cpp - Hoisted diagonal method for matrix-vector multiplication
#include <openfhe.h>
#include <dram_counter.hpp>
#include "utils.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <fstream>
#include <unordered_map>

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
    
    uint32_t numLimbs     = 15;
    uint32_t numDigits    = 1;
    uint32_t scaleModSize = 50;
    uint32_t ringDim      = 65536; 
    std::size_t matrixDim = 128;  // Actual matrix dimension

    CCParams<CryptoContextCKKSRNS> params;
    params.SetMultiplicativeDepth(numLimbs - 1);
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
    print_matrix(M, matrixDim);
    
    // Create input vector (only first matrixDim elements are meaningful)
    std::vector<double> inputVec(numSlots, 0.0);
    for (std::size_t i = 0; i < matrixDim; ++i) {
        inputVec[i] = 0.1 * (i + 1);  // Simple test values
    }
    
    std::cout << "\nInput vector (first " << std::min(matrixDim, std::size_t(10)) << " elements): ";
    for (std::size_t i = 0; i < matrixDim && i < 10; ++i) {
        std::cout << inputVec[i] << " ";
    }
    if (matrixDim > 10) std::cout << "...";
    std::cout << "\n\n";

    // ============================================
    // EXTRACT AND ENCODE NON-EMPTY DIAGONALS
    // ============================================
    
    std::cout << "Extracting diagonals...\n";
    
    // Extract all non-empty diagonals (using positive indices only!)
    auto diagonals = extract_generalized_diagonals(M, matrixDim);
    std::cout << "Found " << diagonals.size() << " non-empty diagonals\n";
    
    // Generate rotation keys only for the diagonals we actually need
    // k=0 doesn't need rotation, so we skip it
    std::vector<int32_t> rotationIndices;
    for (const auto& [k, _] : diagonals) {
        if (k != 0) {
            rotationIndices.push_back(k);
        }
    }
    
    cc->EvalRotateKeyGen(keyPair.secretKey, rotationIndices);
    std::cout << "Generated rotation keys for " << rotationIndices.size() << " rotations\n";
    
    // Encode diagonals as plaintexts
    std::unordered_map<int, Plaintext> diagonalPlaintexts;
    for (const auto& [k, diag] : diagonals) {
        diagonalPlaintexts[k] = cc->MakeCKKSPackedPlaintext(diag);
    }
    
    // Encrypt input vector
    Plaintext inputPtxt = cc->MakeCKKSPackedPlaintext(inputVec);
    auto inputCipher = cc->Encrypt(keyPair.publicKey, inputPtxt);

    // ============================================
    // SERIALIZE
    // ============================================
    
    std::cout << "Serializing keys and input...\n";
    
    std::ofstream rotKeyFile("data/rotation-keys.bin", std::ios::binary);
    if (!cc->SerializeEvalAutomorphismKey(rotKeyFile, SerType::BINARY)) {
        std::cerr << "Failed to serialize rotation keys\n";
        return 1;
    }
    rotKeyFile.close();
    
    if (!Serial::SerializeToFile("data/input.bin", inputCipher, SerType::BINARY)) {
        std::cerr << "Failed to serialize input\n";
        return 1;
    }
    
    cc->ClearEvalAutomorphismKeys();
    inputCipher.reset();

    // ============================================
    // PROFILED SINGLE-HOISTED COMPUTATION
    // ============================================
    
    std::cout << "\nStarting profiled computation...\n\n";
    
    g_dram_counter.start();
    
    // Load keys
    std::ifstream rotKeyIn("data/rotation-keys.bin", std::ios::binary);
    if (!cc->DeserializeEvalAutomorphismKey(rotKeyIn, SerType::BINARY)) {
        std::cerr << "Failed to load rotation keys\n";
        return 1;
    }
    rotKeyIn.close();
    
    // Load input
    Ciphertext<DCRTPoly> cipherInput;
    if (!Serial::DeserializeFromFile("data/input.bin", cipherInput, SerType::BINARY)) {
        std::cerr << "Failed to load input\n";
        return 1;
    }
    
    PIN_MARKER_START();
    
    // === SINGLE-HOISTED DIAGONAL METHOD (Lattigo-style) ===
    // 1. Precompute digit decomposition once (hoisting)
    // 2. Perform all rotations in extended basis QP
    // 3. Accumulate everything in QP space (avoiding intermediate mod-downs)
    // 4. Handle k=0 (no rotation) separately at the end
    // 5. Single KeySwitchDown from QP to Q at the very end
    
    // Step 1: Precompute digit decomposition of the input ciphertext
    // This is equivalent to Lattigo's GadgetProductHoistedLazy preparation
    auto digits = cc->EvalFastRotationPrecompute(cipherInput);
    
    // Step 2: Separate k=0 from other diagonals (like Lattigo's 'state' variable)
    bool hasMainDiagonal = (diagonals.find(0) != diagonals.end());
    std::vector<int> rotationKeys;
    for (const auto& [k, _] : diagonals) {
        if (k != 0) {
            rotationKeys.push_back(k);
        }
    }
    
    // Step 3: Perform all rotations in extended basis (QP space)
    // Using vector for thread-safe parallel access
    std::vector<Ciphertext<DCRTPoly>> rotatedCiphersExt(rotationKeys.size());
    
    // Parallel rotation using precomputed digits
    // The 'false' parameter means we don't compute c0 yet (like Lattigo)
    #pragma omp parallel for
    for (size_t i = 0; i < rotationKeys.size(); ++i) {
        int k = rotationKeys[i];
        // EvalFastRotationExt keeps result in extended basis QP
        // addFirst=false means we skip computing c0 for now (efficiency)
        rotatedCiphersExt[i] = cc->EvalFastRotationExt(cipherInput, k, digits, false);
    }
    
    // Step 4: Multiply by diagonals and accumulate in QP space
    // This avoids intermediate rescaling/mod-down operations
    Ciphertext<DCRTPoly> resultQP;
    bool first = true;
    
    for (size_t i = 0; i < rotationKeys.size(); ++i) {
        int k = rotationKeys[i];
        
        // Multiply rotated ciphertext by diagonal (stays in QP)
        // This multiplication happens in the extended basis
        auto partialQP = cc->EvalMult(rotatedCiphersExt[i], diagonalPlaintexts[k]);
        
        // Accumulate in QP space (no mod-down yet!)
        if (first) {
            resultQP = partialQP;
            first = false;
        } else {
            // EvalAdd in QP space - more efficient than repeated mod-downs
            resultQP = cc->EvalAdd(resultQP, partialQP);
        }
    }
    
    // Step 5: Now scale down from QP to Q (like Lattigo's ModDownQPtoQNTT)
    Ciphertext<DCRTPoly> result;
    if (!first) {
        // KeySwitchDown scales from extended basis P*Q down to Q
        result = cc->KeySwitchDown(resultQP);
    }
    
    // Step 6: Handle main diagonal (k=0) separately - no rotation needed
    // This is done after mod-down, similar to Lattigo's approach
    if (hasMainDiagonal) {
        auto mainDiagResult = cc->EvalMult(cipherInput, diagonalPlaintexts[0]);
        
        if (first) {
            // If we only have the main diagonal
            result = mainDiagResult;
        } else {
            // Add to the already mod-downed result
            result = cc->EvalAdd(result, mainDiagResult);
        }
    }
    
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
    
    // Use the utility function for verification
    verify_matrix_vector_result(resultVec, M, inputVec, matrixDim);
    
    return 0;
}