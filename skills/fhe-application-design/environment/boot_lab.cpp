// fhe-boot-lab — standalone CKKS bootstrap parameter lab (baked into fhe-dev).
//
// Answers ONE question empirically, in minutes: "does a REAL bootstrap work,
// and how accurately, at these parameters?" Run it BEFORE designing any
// bootstrapped circuit, and again whenever a decode fails downstream.
//
// Why it exists (hard-won, C-MAPSS RUL challenge):
//   * OpenFHE's EvalBootstrap is a SILENT NO-OP when the input ciphertext has
//     more remaining levels than a refresh would return. A naive test that
//     bootstraps a fresh ciphertext validates NOTHING — it never bootstraps.
//     This lab burns inputs down with plaintext mults first and labels every
//     refresh [real] or [NO-OP!].
//   * Low scaling sizes that are fine for arithmetic can be fatal for the
//     bootstrap's internal sine machinery (e.g. scaling 38 / first 43 passed
//     keygen and ran a full 30-step circuit, then every real refresh was
//     garbage). Only a real-bootstrap measurement catches this.
//
// Usage:
//   fhe-boot-lab <scaling> <first> <depth> [slots=16384] [dnum=3] [budget=3]
//               [sparse=0] [iters=1] [corrFactor=0(auto)]
// Examples:
//   fhe-boot-lab 45 50 24                       # basic sweep
//   fhe-boot-lab 50 51 24 16384 3 3 1           # sparse-ternary secret
//   fhe-boot-lab 50 51 24 16384 3 3 0 2         # iterative (Meta-BTS) refresh
//   fhe-boot-lab 45 46 24 16384 3 3 0 1 13      # explicit correction factor
// Interpretation:
//   - every swept line should say [real]; a [NO-OP!] means the burn didn't
//     reach the trigger depth (raise depth or lower the level swept)
//   - max error ~2^-10 or better per real refresh, stable across the 5x
//     chain, is a usable configuration; errors near the message scale or
//     Decode failures convict the parameters, not your circuit.

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "openfhe.h"

using namespace lbcrypto;

int main(int argc, char** argv) {
    uint32_t scaling = argc > 1 ? atoi(argv[1]) : 45;
    uint32_t first   = argc > 2 ? atoi(argv[2]) : 50;
    uint32_t depth   = argc > 3 ? atoi(argv[3]) : 24;
    uint32_t slots   = argc > 4 ? atoi(argv[4]) : 16384;
    uint32_t dnumA   = argc > 5 ? atoi(argv[5]) : 3;
    uint32_t budgetA = argc > 6 ? atoi(argv[6]) : 3;
    bool     sparse  = argc > 7 && atoi(argv[7]) != 0;
    uint32_t iters   = argc > 8 ? atoi(argv[8]) : 1;
    uint32_t corrF   = argc > 9 ? atoi(argv[9]) : 0;   // 0 = OpenFHE auto

    std::cout << "== fhe-boot-lab: scaling " << scaling << "  first " << first
              << "  depth " << depth << "  slots " << slots << "  dnum " << dnumA << "  budget " << budgetA
              << (sparse ? "  SPARSE" : "  uniform") << "  iters " << iters
              << "  corrF " << corrF << " ==" << std::endl;

    CCParams<CryptoContextCKKSRNS> p;
    p.SetMultiplicativeDepth(depth);
    p.SetScalingModSize(scaling);
    p.SetFirstModSize(first);
    p.SetScalingTechnique(FLEXIBLEAUTO);
    p.SetSecurityLevel(HEStd_128_classic);
    p.SetSecretKeyDist(sparse ? SPARSE_TERNARY : UNIFORM_TERNARY);
    p.SetRingDim(65536);
    p.SetBatchSize(slots);
    p.SetNumLargeDigits(dnumA);
    auto cc = GenCryptoContext(p);
    cc->Enable(PKE); cc->Enable(KEYSWITCH); cc->Enable(LEVELEDSHE);
    cc->Enable(ADVANCEDSHE); cc->Enable(FHE);
    std::cout << "context OK (ring " << cc->GetRingDimension()
              << "); generating keys (the slow part)..." << std::endl;

    std::vector<uint32_t> lb = {budgetA, budgetA};
    cc->EvalBootstrapSetup(lb, {0, 0}, slots, corrF);
    auto keys = cc->KeyGen();
    cc->EvalMultKeyGen(keys.secretKey);
    cc->EvalBootstrapKeyGen(keys.secretKey, slots);
    std::cout << "keys ready" << std::endl;

    std::vector<double> v(slots);
    for (uint32_t i = 0; i < slots; ++i) v[i] = 0.9 * std::sin(0.001 * i);
    std::vector<double> ones(slots, 1.0);
    auto ct = cc->Encrypt(keys.publicKey,
                          cc->MakeCKKSPackedPlaintext(v, 1, 0, nullptr, slots));

    auto burn_to = [&](Ciphertext<DCRTPoly> x, uint32_t lvl) {
        while (x->GetLevel() < lvl) {
            auto pt1 = cc->MakeCKKSPackedPlaintext(ones, 1, x->GetLevel(), nullptr, slots);
            x = cc->EvalMult(x, pt1);
        }
        return x;
    };
    auto err = [&](const Ciphertext<DCRTPoly>& x) {
        Plaintext pt; cc->Decrypt(keys.secretKey, x, &pt);
        pt->SetLength(slots);
        auto got = pt->GetRealPackedValue();
        double m = 0;
        for (uint32_t i = 0; i < slots; ++i) m = std::max(m, std::abs(got[i] - v[i]));
        return m;
    };

    for (uint32_t lvl : {depth - 8, depth - 5, depth - 3, depth - 2}) {
        try {
            auto deep = burn_to(ct, lvl);
            auto b = (iters > 1) ? cc->EvalBootstrap(deep, iters, 3) : cc->EvalBootstrap(deep);
            bool real = b->GetLevel() != deep->GetLevel();
            double e = err(b);
            std::cout << "boot from level " << lvl << " -> " << b->GetLevel()
                      << (real ? "  [real]  " : "  [NO-OP!] ")
                      << "max error " << e << "  (~2^"
                      << (int)std::round(std::log2(e)) << ")" << std::endl;
        } catch (const std::exception& e) {
            std::cout << "boot from level " << lvl << ": FAIL (" << e.what() << ")"
                      << std::endl;
        }
    }
    try {
        auto x = burn_to(ct, depth - 3);
        for (int i = 0; i < 5; ++i) {
            x = (iters > 1) ? cc->EvalBootstrap(x, iters, 3) : cc->EvalBootstrap(x);
            x = burn_to(x, depth - 3);
        }
        std::cout << "5x chained real bootstraps: max error " << err(x) << std::endl;
    } catch (const std::exception& e) {
        std::cout << "chained: FAIL (" << e.what() << ")" << std::endl;
    }
    return 0;
}
