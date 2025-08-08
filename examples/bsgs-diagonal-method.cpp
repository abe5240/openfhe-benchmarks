// BSGS diagonal method (non-hoisted) with n1 = pow2_ceil(sqrt(#nonzero-diagonals))
// - Works when matrixDim << numSlots
// - Signed diagonal indexing; skips empty giant blocks
// - Baby rotations cached across all giant steps (EvalRotate, no hoisting)
// - Rotation keys generated only as needed, serialized per-index

#include <openfhe.h>
#include <dram_counter.hpp>
#include "utils.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#include <ciphertext-ser.h>
#include <cryptocontext-ser.h>
#include <key/key-ser.h>
#include <scheme/ckksrns/ckksrns-ser.h>

using namespace lbcrypto;

// ----- PIN markers -----
extern "C" {
void __attribute__((noinline)) PIN_MARKER_START() { asm volatile(""); }
void __attribute__((noinline)) PIN_MARKER_END()   { asm volatile(""); }
}

// ----- DRAM counter -----
static DRAMCounter g_dram_counter;

// ---------- helpers ----------
static inline int floordiv(int a, int b) {
    int q = a / b, r = a % b;
    if (r && ((a < 0) != (b < 0))) --q;
    return q;
}
static inline int normalize_signed_k(int k, int slots) {
    int half = slots / 2;
    return (k <= half) ? k : k - slots;
}
static void gen_and_save_rotkey(const CryptoContext<DCRTPoly>& cc,
                                const PrivateKey<DCRTPoly>& sk,
                                int rot, const std::string& path) {
    cc->EvalRotateKeyGen(sk, {rot});
    std::ofstream f(path, std::ios::binary);
    if (!cc->SerializeEvalAutomorphismKey(f, SerType::BINARY))
        throw std::runtime_error("Serialize rotkey " + std::to_string(rot));
    f.close();
    cc->ClearEvalAutomorphismKeys();
}
static void load_rotkey(const CryptoContext<DCRTPoly>& cc,
                        int rot, const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!cc->DeserializeEvalAutomorphismKey(f, SerType::BINARY))
        throw std::runtime_error("Deserialize rotkey " + std::to_string(rot));
    f.close();
}
static inline int pow2_ceil(double x) {
    if (x <= 1.0) return 1;
    int p = 1;
    while (p < (int)std::ceil(x)) p <<= 1;
    return p;
}

int main() {
    if (!g_dram_counter.init())
        std::cerr << "Warning: DRAM measurements disabled\n";

    // ---- CKKS params (adjust as needed) ----
    uint32_t numLimbs     = 4;
    uint32_t numDigits    = 1;
    uint32_t scaleModSize = 50;
    uint32_t ringDim      = 2048;     // example: slots = ringDim/2
    std::size_t matrixDim = 16;

    CCParams<CryptoContextCKKSRNS> params;
    params.SetMultiplicativeDepth(numLimbs - 1);
    params.SetScalingModSize(scaleModSize);
    params.SetRingDim(ringDim);
    params.SetScalingTechnique(FIXEDMANUAL);
    params.SetKeySwitchTechnique(HYBRID);
    params.SetNumLargeDigits(numDigits);
    params.SetSecurityLevel(HEStd_NotSet);

    auto cc = GenCryptoContext(params);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);

    const int slots = (int)cc->GetEncodingParams()->GetBatchSize();
    if ((int)matrixDim > slots) {
        std::cerr << "matrixDim must be <= slots\n";
        return 1;
    }

    std::cout << "=== BSGS (Diagonal, non-hoisted) ===\n";
    std::cout << "matrixDim=" << matrixDim << "  slots=" << slots
              << "  ringDim=" << ringDim << "\n";

    auto kp = cc->KeyGen();

    auto M = make_embedded_random_matrix(matrixDim, slots);
    auto v = make_random_input_vector(matrixDim, slots);
    print_matrix(M, matrixDim);

    // ---- extract diagonals ----
    auto diags_raw = extract_generalized_diagonals(M, matrixDim);
    std::cout << "non-empty diagonals: " << diags_raw.size() << "\n";

    // map to signed k ∈ (−slots/2, slots/2]
    std::map<int, std::vector<double>> diags_signed;
    for (const auto& kv : diags_raw) {
        int k_signed = normalize_signed_k(kv.first, slots);
        diags_signed[k_signed] = kv.second;
    }
    const std::size_t D = diags_signed.size();      // ≈ 2*matrixDim-1

    // ---- n1 = pow2_ceil(sqrt(D)); n2≈ceil(slots/n1) (informational) ----
    int n1 = pow2_ceil(std::sqrt((double)D));
    if (n1 > slots) n1 = slots;
    if (n1 < 1)     n1 = 1;
    int n2 = (int)std::ceil((double)slots / n1);
    std::cout << "chosen n1=" << n1 << " (pow2 ceil sqrt(D)), n2≈" << n2 << "\n";

    // ---- pre-shift + encode ----
    std::map<int, Plaintext> encDiag; // key = signed k
    std::set<int> usedResidues;       // i in [0, n1)
    std::set<int> usedJ;              // j may be negative

    for (const auto& kv : diags_signed) {
        int k = kv.first;             // signed
        int j = floordiv(k, n1);
        int i = k - j * n1;           // 0..n1-1

        usedResidues.insert(i);
        usedJ.insert(j);

        auto shifted = rotateVectorDown(kv.second, (n1 * j) % slots);
        encDiag[k] = cc->MakeCKKSPackedPlaintext(shifted);
    }

    // ---- rotation keys: baby (i>0) and giant (j!=0) that actually occur ----
    std::set<int> rotIdx;
    for (int i : usedResidues) if (i != 0) rotIdx.insert(i);
    for (int j : usedJ) {
        int r = n1 * j; if (r != 0) rotIdx.insert(r);
    }
    std::cout << "generating " << rotIdx.size() << " rotation keys...\n";
    for (int r : rotIdx) {
        gen_and_save_rotkey(cc, kp.secretKey, r, "data/rot_key_" + std::to_string(r) + ".bin");
    }

    // ---- encrypt & serialize input ----
    auto ctIn = cc->Encrypt(kp.publicKey, cc->MakeCKKSPackedPlaintext(v));
    Serial::SerializeToFile("data/input.bin", ctIn, SerType::BINARY);
    ctIn.reset();

    // ---- reload ----
    Ciphertext<DCRTPoly> in;
    Serial::DeserializeFromFile("data/input.bin", in, SerType::BINARY);

    // ---- compute (profiled) ----
    std::cout << "\nStarting profiled BSGS compute...\n";
    g_dram_counter.start();
    PIN_MARKER_START();

    // cache baby rotations on-demand (non-hoisted: EvalRotate)
    std::vector<Ciphertext<DCRTPoly>> baby(n1);
    std::vector<bool> haveBaby(n1, false);
    baby[0] = in; haveBaby[0] = true;

    auto getBaby = [&](int i) -> Ciphertext<DCRTPoly> {
        if (!haveBaby[i]) {
            std::string path = "data/rot_key_" + std::to_string(i) + ".bin";
            load_rotkey(cc, i, path);
            baby[i] = cc->EvalRotate(in, i);
            cc->ClearEvalAutomorphismKeys();
            haveBaby[i] = true;
        }
        return baby[i];
    };

    // only iterate actually-used giant blocks
    std::vector<int> jList(usedJ.begin(), usedJ.end());
    std::sort(jList.begin(), jList.end());

    Ciphertext<DCRTPoly> result;
    bool haveResult = false;

    for (int j : jList) {
        Ciphertext<DCRTPoly> block;
        bool haveBlock = false;

        for (int i = 0; i < n1; ++i) {
            int k = j * n1 + i;
            auto it = encDiag.find(k);
            if (it == encDiag.end()) continue;

            auto ctB = (i == 0) ? baby[0] : getBaby(i);
            auto prod = cc->EvalMult(ctB, it->second);

            if (!haveBlock) { block = prod; haveBlock = true; }
            else            { block = cc->EvalAdd(block, prod); }
        }

        if (!haveBlock) continue;

        int rot = n1 * j;
        if (rot != 0) {
            std::string path = "data/rot_key_" + std::to_string(rot) + ".bin";
            load_rotkey(cc, rot, path);
            block = cc->EvalRotate(block, rot);
            cc->ClearEvalAutomorphismKeys();
        }

        if (!haveResult) { result = block; haveResult = true; }
        else             { result = cc->EvalAdd(result, block); }
    }

    // keep scale tidy
    result = cc->Rescale(result);

    PIN_MARKER_END();
    g_dram_counter.stop();
    g_dram_counter.print_results(true);

    Serial::SerializeToFile("data/result.bin", result, SerType::BINARY);

    // ---- verify ----
    Plaintext out;
    cc->Decrypt(kp.secretKey, result, &out);
    out->SetLength(slots);
    verify_matrix_vector_result(out->GetRealPackedValue(), M, v, matrixDim);

    return 0;
}