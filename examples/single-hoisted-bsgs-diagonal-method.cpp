// examples/single-hoisted-bsgs-diagonal-method.cpp - Hoisted BSGS diagonal method for matrix-vector multiplication
#include <openfhe.h>
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

using namespace lbcrypto;

int main(int argc, char* argv[]) {
    // Parse arguments
    ArgParser parser;
    parser.parse(argc, argv);
    
    // Get parameters
    bool debug = parser.getDebug();
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

    int numSlots = static_cast<int>(cc->GetEncodingParams()->GetBatchSize());
    if (static_cast<int>(matrixDim) > numSlots) {
        std::cerr << "Error: matrixDim (" << matrixDim << ") must be <= numSlots (" << numSlots << ")\n";
        return 1;
    }
    
    if (debug) {
        std::cout << "=== Single-Hoisted BSGS Method with On-Demand Key Loading ===\n";
        std::cout << "Actual matrix dimension: " << matrixDim << "×" << matrixDim << "\n";
        std::cout << "Number of slots: " << numSlots << "\n";
        std::cout << "Ring dimension: " << params.ringDim << "\n";
        std::cout << "Multiplicative depth: " << params.multDepth << "\n\n";
    }

    // Generate key pair
    auto keyPair = cc->KeyGen();
    
    // CREATE MATRIX AND VECTOR
    auto M = make_embedded_random_matrix(matrixDim, numSlots);
    auto inputVec = make_random_input_vector(matrixDim, numSlots);

    // EXTRACT DIAGONALS AND CONVERT TO SIGNED INDEXING
    if (debug) {
        std::cout << "Extracting diagonals...\n";
    }
    
    // First extract diagonals with regular indexing
    auto diagonalsUnsigned = extract_generalized_diagonals(M, matrixDim);
    
    // Convert to signed indexing
    std::map<int, std::vector<double>> diagonalsSigned;
    
    for (const auto& entry : diagonalsUnsigned) {
        int kUnsigned = entry.first;
        int kSigned = normalizeToSignedIndex(kUnsigned, numSlots);
        diagonalsSigned[kSigned] = entry.second;
    }
    
    int numDiagonals = static_cast<int>(diagonalsSigned.size());
    if (debug) {
        std::cout << "Found " << numDiagonals << " non-empty diagonals\n";
        std::cout << "Diagonal indices range from " << diagonalsSigned.begin()->first 
                  << " to " << diagonalsSigned.rbegin()->first << "\n";
    }
    
    // BSGS PARAMETERS
    int n1 = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(numDiagonals))));
    
    if (n1 < 1) n1 = 1;
    if (n1 > numSlots) n1 = numSlots;
    
    int n2_approx = static_cast<int>(std::ceil(static_cast<double>(numSlots) / n1));
    
    if (debug) {
        std::cout << "BSGS parameters: n1 = " << n1 
                  << " (based on sqrt(" << numDiagonals << ")), n2 ≈ " << n2_approx << "\n";
    }
    
    // DECOMPOSE DIAGONALS AND PRE-SHIFT
    if (debug) {
        std::cout << "Pre-shifting diagonals for BSGS decomposition...\n";
    }
    
    std::set<int> usedBabySteps;
    std::set<int> usedGiantSteps;
    
    // Pre-shift each diagonal by its giant step amount
    std::map<int, Plaintext> preshiftedDiagonals;
    
    for (const auto& entry : diagonalsSigned) {
        int k = entry.first;
        
        int j = floorDivision(k, n1);
        int i = k - j * n1;
        
        usedBabySteps.insert(i);
        usedGiantSteps.insert(j);
        
        // Pre-shift the diagonal
        auto diagonal = entry.second;
        int shiftAmount = (n1 * j) % numSlots;
        if (shiftAmount < 0) shiftAmount += numSlots;
        diagonal = rotateVectorDown(diagonal, shiftAmount);
        
        preshiftedDiagonals[k] = cc->MakeCKKSPackedPlaintext(diagonal);
    }
    
    if (debug) {
        std::cout << "Baby steps used: " << usedBabySteps.size() 
                  << ", Giant steps used: " << usedGiantSteps.size() << "\n";
        std::cout << "Giant step range: [" << *usedGiantSteps.begin() 
                  << ", " << *usedGiantSteps.rbegin() << "]\n";
    }
    
    // CREATE TEMPORARY DIRECTORY
    TempDirectory tempDir;
    if (!tempDir.isValid()) {
        std::cerr << "Failed to create temporary directory" << std::endl;
        return 1;
    }
    
    // GENERATE AND SAVE ROTATION KEYS INDIVIDUALLY
    if (debug) {
        std::cout << "Generating and saving rotation keys individually...\n";
    }
    
    std::set<int> rotationIndices;
    
    // Baby rotations
    for (int i : usedBabySteps) {
        if (i != 0) rotationIndices.insert(i);
    }
    
    // Giant rotations
    for (int j : usedGiantSteps) {
        if (j != 0) {
            int rotation = n1 * j;
            rotationIndices.insert(rotation);
        }
    }
    
    // Generate and save each rotation key separately
    for (int rot : rotationIndices) {
        cc->EvalRotateKeyGen(keyPair.secretKey, {rot});
        
        std::stringstream filename;
        filename << "hoisted-bsgs-rot-key-" << rot << ".bin";
        std::string keyPath = tempDir.getFilePath(filename.str());
        
        std::ofstream keyFile(keyPath, std::ios::binary);
        if (!cc->SerializeEvalAutomorphismKey(keyFile, SerType::BINARY)) {
            std::cerr << "Failed to save rotation key " << rot << "\n";
            return 1;
        }
        keyFile.close();
        cc->ClearEvalAutomorphismKeys();
    }
    
    if (debug) {
        std::cout << "Generated and saved " << rotationIndices.size() << " rotation keys\n";
    }
    
    // ENCRYPT AND SERIALIZE INPUT
    if (debug) {
        std::cout << "Encrypting and serializing input...\n";
    }
    
    Plaintext inputPtxt = cc->MakeCKKSPackedPlaintext(inputVec);
    auto inputCipher = cc->Encrypt(keyPair.publicKey, inputPtxt);
    
    std::string inputPath = tempDir.getFilePath("input.bin");
    if (!Serial::SerializeToFile(inputPath, inputCipher, SerType::BINARY)) {
        std::cerr << "Failed to serialize input\n";
        return 1;
    }
    
    inputCipher.reset();

    // PROFILED HOISTED BSGS COMPUTATION WITH ON-DEMAND KEY LOADING
    if (debug) {
        std::cout << "\nStarting profiled hoisted BSGS computation with on-demand key loading...\n\n";
    }
    
    // Start DRAM measurement
    measurement.startDRAM();
    
    // Load input ciphertext
    Ciphertext<DCRTPoly> cipherInput;
    if (!Serial::DeserializeFromFile(inputPath, cipherInput, SerType::BINARY)) {
        std::cerr << "Failed to load input\n";
        return 1;
    }
    
    // PIN markers around the computation
    measurement.startPIN();
    
    // SINGLE-HOISTED BSGS WITH ON-DEMAND KEY LOADING
    
    // Step 1: Precompute rotation digits once (hoisting optimization)
    if (debug) {
        std::cout << "Precomputing rotation digits for hoisting...\n";
    }
    auto precomputedDigits = cc->EvalFastRotationPrecompute(cipherInput);
    
    // Step 2: Get cyclotomic order
    uint32_t cyclotomicOrder = 2 * cc->GetRingDimension();
    
    // Step 3: Cache for baby rotations with on-demand computation
    std::vector<Ciphertext<DCRTPoly>> babyRotationCache(n1);
    std::vector<bool> babyRotationComputed(n1, false);
    
    // Identity rotation is always available
    babyRotationCache[0] = cipherInput;
    babyRotationComputed[0] = true;
    
    // Helper lambda to get/compute baby rotation with hoisting
    auto getHoistedBabyRotation = [&](int i) -> const Ciphertext<DCRTPoly>& {
        if (!babyRotationComputed[i]) {
            // Load rotation key for this baby step
            std::stringstream filename;
            filename << "hoisted-bsgs-rot-key-" << i << ".bin";
            std::string keyPath = tempDir.getFilePath(filename.str());
            
            std::ifstream keyFile(keyPath, std::ios::binary);
            if (!cc->DeserializeEvalAutomorphismKey(keyFile, SerType::BINARY)) {
                std::cerr << "Failed to load key for baby step " << i << "\n";
                throw std::runtime_error("Missing rotation key");
            }
            keyFile.close();
            
            // Compute rotation using hoisting
            babyRotationCache[i] = cc->EvalFastRotation(cipherInput, i, cyclotomicOrder, precomputedDigits);
            
            // Clear the key immediately after use
            cc->ClearEvalAutomorphismKeys();
            babyRotationComputed[i] = true;
        }
        return babyRotationCache[i];
    };
    
    // Step 4: Process giant steps in sorted order
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
            
            // Get baby rotation (identity or compute with hoisting)
            const auto& babyRotated = (i == 0) ? cipherInput : getHoistedBabyRotation(i);
            
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
        
        // Apply giant rotation if j ≠ 0 (load key on-demand)
        if (j != 0) {
            int giantRotation = n1 * j;
            
            // Load rotation key for this giant step
            std::stringstream filename;
            filename << "hoisted-bsgs-rot-key-" << giantRotation << ".bin";
            std::string keyPath = tempDir.getFilePath(filename.str());
            
            std::ifstream keyFile(keyPath, std::ios::binary);
            if (!cc->DeserializeEvalAutomorphismKey(keyFile, SerType::BINARY)) {
                std::cerr << "Failed to load key for giant step " << giantRotation << "\n";
                return 1;
            }
            keyFile.close();
            
            // Note: Giant steps use regular rotation (not hoisted)
            giantBlockSum = cc->EvalRotate(giantBlockSum, giantRotation);
            
            // Clear the key immediately after use
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
    
    // Always verify
    if (debug) {
        std::cout << "\nDecrypting and verifying result...\n";
    }
    
    Plaintext resultPtxt;
    cc->Decrypt(keyPair.secretKey, result, &resultPtxt);
    resultPtxt->SetLength(numSlots);
    
    auto resultVec = resultPtxt->GetRealPackedValue();
    
    // Verify and return exit code
    return verify_matrix_vector_result(resultVec, M, inputVec, matrixDim, debug) ? 0 : 1;
}