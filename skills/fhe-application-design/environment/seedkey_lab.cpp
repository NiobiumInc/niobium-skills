// fhe-seedkey-lab — application-owned seeded key generation probe (baked into
// fhe-dev). Companion to fhe-boot-lab: it answers ONE question empirically, in
// minutes: "if the application generates its own public / relinearization /
// rotation keys with a seeded `a` half, and the server re-expands `a` from the
// seed, are the resulting keys bit-identical on both sides and do they compute
// the same thing as the library's keys?"
//
// Why it exists (Stage 11, optimization A2 / B3 in
// references/optimizing-the-running-application.md):
//   * Mainline OpenFHE has no seeded-key API. The library samples the public
//     polynomial `a` INSIDE key generation and derives `b` from it, so the only
//     way to ship half-size keys is for the application to run key generation
//     itself: choose `a` from its own seeded PRNG, compute `b` exactly as the
//     library would, and let the server regenerate `a` from the seed.
//   * That is a transcription of the library's key-switch formula over the
//     extended modulus Q·P. A transcription error does not raise; it produces
//     garbage at decrypt. This lab is the evidence that the transcription is
//     right for the installed library version — run it before relying on A2,
//     and again after every SDK upgrade.
//
// What it does, in order:
//   1. Library path: KeyGen, EvalMultKeyGen, EvalRotateKeyGen as usual.
//   2. Application path ("client"): the same three key kinds, but with `a`
//      drawn from a Blake2 engine seeded by (seed, per-object counter) — a
//      PRNG that is NOT the library's global one (which also draws s and e).
//   3. "Ship" only the `b` halves + the seed; the "server" re-expands `a` and
//      the lab checks the expanded polynomials are IDENTICAL to the client's.
//   4. Functional check: encrypt a known vector under both public keys, rotate
//      and square-with-relinearize under both key sets (and crosswise: library
//      ciphertext under application keys, and vice versa), decrypt, and report
//      the max error of each against the exact answer.
//   5. Report serialized sizes: full key vs `b`-halves-only.
//
// Usage:
//   fhe-seedkey-lab [scaling=45] [first=55] [depth=8] [ringDim=16384] [dnum=3]
//                   [rotations=1,2,4,8] [seed=42] [secure=0]
//   secure=1 sets HEStd_128_classic (then ringDim must be large enough);
//   secure=0 uses HEStd_NotSet so the lab runs fast at a small toy ring.
// Examples:
//   fhe-seedkey-lab                              # toy ring, seconds
//   fhe-seedkey-lab 45 55 12 65536 3 1,2,4,8,16 7 1   # production-shaped
// Interpretation:
//   - "expanded a: IDENTICAL" for every key is the wire-format check; any
//     DIFFERENT line is a seed/counter/order bug on one side.
//   - the four error lines (lib/lib, app/app, lib-ct/app-keys, app-ct/lib-keys)
//     should all sit at the same noise floor; an app line orders of magnitude
//     above the lib line convicts the b-formula transcription for THIS library
//     version, not the idea.
//   - exit status 0 = PASS (identity exact, errors within tolerance).

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "openfhe.h"
#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "scheme/ckksrns/ckksrns-ser.h"
#include "utils/prng/blake2engine.h"

using namespace lbcrypto;
using default_prng::Blake2Engine;  // the seeded engine lives in the top-level default_prng namespace

namespace {

using SeedArray = Blake2Engine::blake2_seed_array_t;

// Derive the 512-bit engine seed from a small integer. In an application the
// seed is 64 random bytes from the OS; the derivation here only makes the lab
// reproducible from the command line.
SeedArray MakeSeed(uint64_t s) {
    SeedArray seed{};
    for (size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<uint32_t>((s * 0x9E3779B97F4A7C15ULL + i * 0xD1B54A32D192ED03ULL) >> (i % 32));
    return seed;
}

// Uniform polynomial over `params`, in EVALUATION form, from (seed, counter).
// Uniform in NTT form is uniform in coefficient form, so this is exactly what
// the library's DiscreteUniformGenerator produces — just reproducible.
// Rejection sampling keeps each limb unbiased modulo its own prime; the limb
// order and coefficient order are the wire format and must never change.
DCRTPoly SeededUniform(const std::shared_ptr<ILDCRTParams<BigInteger>>& params, const SeedArray& seed,
                       uint64_t counter) {
    Blake2Engine eng(seed, counter);
    DCRTPoly out(params, Format::EVALUATION, true);
    const auto& ps = params->GetParams();
    for (size_t i = 0; i < ps.size(); ++i) {
        const uint64_t q   = ps[i]->GetModulus().ConvertToInt();
        const uint32_t n   = ps[i]->GetRingDimension();
        const uint64_t lim = (UINT64_MAX / q) * q;  // largest multiple of q that fits
        NativeVector v(n, ps[i]->GetModulus());
        for (uint32_t j = 0; j < n; ++j) {
            uint64_t r;
            do {
                r = (static_cast<uint64_t>(eng()) << 32) | static_cast<uint64_t>(eng());
            } while (r >= lim);
            v[j] = NativeInteger(r % q);
        }
        NativePoly p(ps[i], Format::EVALUATION, true);
        p.SetValues(std::move(v), Format::EVALUATION);
        out.SetElementAtIndex(i, std::move(p));
    }
    return out;
}

// Mirror of KeySwitchHYBRID::KeySwitchGenInternal (single-key path) with the
// uniform `a` supplied by the caller. Switches oldKey -> newKey, both over Q:
// relin is (s^2 -> s); a rotation key for automorphism g is (s -> s(X^(g^-1))).
EvalKey<DCRTPoly> SeededKeySwitchGen(const CryptoContext<DCRTPoly>& cc, const DCRTPoly& sOld, const DCRTPoly& sNew,
                                     const std::string& keyTag, const SeedArray& seed, uint64_t counterBase,
                                     std::vector<DCRTPoly>* aOut) {
    const auto cryptoParams = std::dynamic_pointer_cast<CryptoParametersRNS>(cc->GetCryptoParameters());
    const auto& paramsQ     = cryptoParams->GetElementParams();
    const auto& paramsQP    = cryptoParams->GetParamsQP();
    const auto& pparamsQP   = paramsQP->GetParams();

    // Extend sNew from Q to QP exactly as the library does.
    DCRTPoly sNewExt(paramsQP, Format::EVALUATION, true);
    auto sNew0 = sNew.GetElementAtIndex(0);
    sNew0.SetFormat(Format::COEFFICIENT);
    const uint32_t sizeQ  = paramsQ->GetParams().size();
    const uint32_t sizeQP = pparamsQP.size();
    for (uint32_t i = 0; i < sizeQP; ++i) {
        if (i < sizeQ) {
            auto tmp = sNew.GetElementAtIndex(i);
            tmp.SetFormat(Format::EVALUATION);
            sNewExt.SetElementAtIndex(i, std::move(tmp));
        }
        else {
            auto tmp = sNew0;
            tmp.SwitchModulus(pparamsQP[i]->GetModulus(), pparamsQP[i]->GetRootOfUnity(), 0, 0);
            tmp.SetFormat(Format::EVALUATION);
            sNewExt.SetElementAtIndex(i, std::move(tmp));
        }
    }

    const auto ns              = cryptoParams->GetNoiseScale();
    const uint32_t numPerPartQ = cryptoParams->GetNumPerPartQ();
    const uint32_t numPartQ    = cryptoParams->GetNumPartQ();
    const auto& PModq          = cryptoParams->GetPModq();
    auto dgg                   = cryptoParams->GetDiscreteGaussianGenerator();  // library noise, NOT seeded

    std::vector<DCRTPoly> av(numPartQ);
    std::vector<DCRTPoly> bv(numPartQ);
    for (uint32_t part = 0; part < numPartQ; ++part) {
        DCRTPoly a = SeededUniform(paramsQP, seed, counterBase + part);  // the only seeded draw
        DCRTPoly e(dgg, paramsQP, Format::EVALUATION);
        DCRTPoly b(paramsQP, Format::EVALUATION, true);

        const uint32_t startPartIdx = numPerPartQ * part;
        const uint32_t endPartIdx   = (sizeQ > (startPartIdx + numPerPartQ)) ? (startPartIdx + numPerPartQ) : sizeQ;
        for (uint32_t i = 0; i < sizeQP; ++i) {
            const auto& ai  = a.GetElementAtIndex(i);
            const auto& ei  = e.GetElementAtIndex(i);
            const auto& sni = sNewExt.GetElementAtIndex(i);
            if (i < startPartIdx || i >= endPartIdx) {
                b.SetElementAtIndex(i, (-ai * sni) + (ns * ei));
            }
            else {
                const auto& soi = sOld.GetElementAtIndex(i);
                b.SetElementAtIndex(i, (-ai * sni) + (ns * ei) + (PModq[i] * soi));
            }
        }
        av[part] = std::move(a);
        bv[part] = std::move(b);
    }
    if (aOut)
        *aOut = av;
    auto ek = std::make_shared<EvalKeyRelinImpl<DCRTPoly>>(cc);
    ek->SetAVector(std::move(av));
    ek->SetBVector(std::move(bv));
    ek->SetKeyTag(keyTag);
    return ek;
}

// Server side of A2: rebuild a key from its b halves + (seed, counterBase).
EvalKey<DCRTPoly> ExpandKeySwitchKey(const CryptoContext<DCRTPoly>& cc, const std::vector<DCRTPoly>& bv,
                                     const std::string& keyTag, const SeedArray& seed, uint64_t counterBase) {
    const auto cryptoParams = std::dynamic_pointer_cast<CryptoParametersRNS>(cc->GetCryptoParameters());
    const auto& paramsQP    = cryptoParams->GetParamsQP();
    std::vector<DCRTPoly> av(bv.size());
    for (size_t part = 0; part < bv.size(); ++part)
        av[part] = SeededUniform(paramsQP, seed, counterBase + part);
    auto ek = std::make_shared<EvalKeyRelinImpl<DCRTPoly>>(cc);
    ek->SetAVector(std::move(av));
    ek->SetBVector(bv);
    ek->SetKeyTag(keyTag);
    return ek;
}

// The secret a rotation key for automorphism index g switches TO. Mirror of
// LeveledSHEBase::EvalAutomorphismKeyGen: the key is KeySwitchGen(old = s,
// new = s(X^(g^-1))); EvalAutomorphism key-switches FIRST and applies the
// automorphism g afterwards, which brings the result back under s. Getting
// this pairing backwards produces keys that expand identically and decrypt
// to garbage — the exact failure this lab exists to catch.
DCRTPoly RotationTargetSecret(const DCRTPoly& s, uint32_t g) {
    const uint32_t N = s.GetRingDimension();
    const uint32_t M = s.GetCyclotomicOrder();
    uint32_t gi      = NativeInteger(g).ModInverse(M).ConvertToInt<uint32_t>();
    std::vector<uint32_t> vec(N);
    PrecomputeAutoMap(N, gi, &vec);
    return s.AutomorphismTransform(gi, vec);
}

template <class T>
size_t SerializedBytes(const T& obj) {
    std::ostringstream os;
    Serial::Serialize(obj, os, SerType::BINARY);
    return os.str().size();
}

uint64_t Checksum(const DCRTPoly& p) {
    uint64_t h = 1469598103934665603ULL;
    for (const auto& limb : p.GetAllElements()) {
        const auto& vals = limb.GetValues();
        for (uint32_t j = 0; j < vals.GetLength(); ++j) {
            h ^= vals[j].ConvertToInt();
            h *= 1099511628211ULL;
        }
    }
    return h;
}

bool SameVectors(const std::vector<DCRTPoly>& x, const std::vector<DCRTPoly>& y) {
    if (x.size() != y.size())
        return false;
    for (size_t i = 0; i < x.size(); ++i)
        if (!(x[i] == y[i]))
            return false;
    return true;
}

double MaxAbsErr(const std::vector<double>& got, const std::vector<double>& want, size_t n) {
    double m = 0;
    for (size_t i = 0; i < n; ++i)
        m = std::max(m, std::fabs(got[i] - want[i]));
    return m;
}

std::vector<int32_t> ParseRotations(const std::string& s) {
    std::vector<int32_t> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (!tok.empty())
            out.push_back(std::stoi(tok));
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t scaling = argc > 1 ? atoi(argv[1]) : 45;
    uint32_t first   = argc > 2 ? atoi(argv[2]) : 55;
    uint32_t depth   = argc > 3 ? atoi(argv[3]) : 8;
    uint32_t ringDim = argc > 4 ? atoi(argv[4]) : 16384;
    uint32_t dnum    = argc > 5 ? atoi(argv[5]) : 3;
    auto rotations   = ParseRotations(argc > 6 ? argv[6] : "1,2,4,8");
    uint64_t seedInt = argc > 7 ? strtoull(argv[7], nullptr, 10) : 42;
    bool secure      = argc > 8 && atoi(argv[8]) != 0;

    std::cout << "== fhe-seedkey-lab: scaling " << scaling << "  first " << first << "  depth " << depth
              << "  ring " << ringDim << "  dnum " << dnum << "  rotations " << rotations.size() << "  seed "
              << seedInt << (secure ? "  HEStd_128_classic" : "  HEStd_NotSet (toy)") << " ==" << std::endl;

    CCParams<CryptoContextCKKSRNS> p;
    p.SetMultiplicativeDepth(depth);
    p.SetScalingModSize(scaling);
    p.SetFirstModSize(first);
    p.SetScalingTechnique(FLEXIBLEAUTO);
    p.SetKeySwitchTechnique(HYBRID);
    p.SetNumLargeDigits(dnum);
    p.SetRingDim(ringDim);
    p.SetSecurityLevel(secure ? HEStd_128_classic : HEStd_NotSet);
    p.SetBatchSize(ringDim / 2);
    auto cc = GenCryptoContext(p);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);
    cc->Enable(ADVANCEDSHE);

    const uint32_t slots = ringDim / 2;
    const auto cryptoParams = std::dynamic_pointer_cast<CryptoParametersRNS>(cc->GetCryptoParameters());
    std::cout << "   Q limbs " << cryptoParams->GetElementParams()->GetParams().size() << "  QP limbs "
              << cryptoParams->GetParamsQP()->GetParams().size() << "  digits " << cryptoParams->GetNumPartQ()
              << std::endl;

    // ---------------------------------------------------------------- 1. library keys
    auto keys = cc->KeyGen();
    cc->EvalMultKeyGen(keys.secretKey);
    cc->EvalRotateKeyGen(keys.secretKey, rotations);
    const std::string keyTag = keys.secretKey->GetKeyTag();
    const DCRTPoly& s        = keys.secretKey->GetPrivateElement();

    // Snapshot the library's keys so they can be re-installed later.
    auto libRelin = CryptoContextImpl<DCRTPoly>::GetAllEvalMultKeys()[keyTag];
    auto libRotMap = std::make_shared<std::map<uint32_t, EvalKey<DCRTPoly>>>(
        CryptoContextImpl<DCRTPoly>::GetEvalAutomorphismKeyMap(keyTag));

    // ---------------------------------------------------------------- 2. application keys (client)
    const SeedArray seed = MakeSeed(seedInt);
    // Counter plan (part of the wire format): pk = 0; relin parts start at 1;
    // rotation key for automorphism index g starts at 1000 + 64*g.
    const auto& paramsPK = cryptoParams->GetParamsPK();
    const auto& paramsQ  = cryptoParams->GetElementParams();
    if (paramsPK->GetParams().size() != paramsQ->GetParams().size()) {
        std::cerr << "   public-key params have extra limbs (PRE mode?); this lab assumes paramsPK == Q" << std::endl;
        return 2;
    }
    // pk: b = ns*e - a*s  (mirror of PKEBase::KeyGenInternal), a from the seeded expander
    DCRTPoly pkA = SeededUniform(paramsPK, seed, 0);
    DCRTPoly pkE(cryptoParams->GetDiscreteGaussianGenerator(), paramsPK, Format::EVALUATION);
    NativeInteger ns = cryptoParams->GetNoiseScale();
    DCRTPoly pkB(std::move((pkE *= ns) -= (pkA * s)));
    auto appPk = std::make_shared<PublicKeyImpl<DCRTPoly>>(cc);
    appPk->SetPublicElements({pkB, pkA});
    appPk->SetKeyTag(keyTag);

    std::vector<DCRTPoly> relinA;
    auto appRelin = SeededKeySwitchGen(cc, s * s, s, keyTag, seed, 1, &relinA);

    std::map<uint32_t, EvalKey<DCRTPoly>> appRot;
    std::map<uint32_t, std::vector<DCRTPoly>> rotA;
    std::map<uint32_t, int32_t> rotOf;
    for (int32_t r : rotations) {
        uint32_t g = cc->FindAutomorphismIndex(r);
        rotOf[g]   = r;
        appRot[g]  = SeededKeySwitchGen(cc, s, RotationTargetSecret(s, g), keyTag, seed, 1000 + 64ULL * g, &rotA[g]);
    }

    // ---------------------------------------------------------------- 2b. formula check against the library's keys
    // Re-derive b from the library's OWN a with our formula; b_lib - b_ours = ns*(e_lib - e_ours),
    // so a correct transcription leaves only noise-sized centered coefficients.
    auto formulaCheck = [&](const std::string& label, const EvalKey<DCRTPoly>& libKey, const DCRTPoly& sOld, const DCRTPoly& sNewIn) {
        const auto& paramsQP   = cryptoParams->GetParamsQP();
        const auto& pparamsQP  = paramsQP->GetParams();
        const uint32_t sizeQ   = paramsQ->GetParams().size();
        const uint32_t sizeQP  = pparamsQP.size();
        DCRTPoly sNewExt(paramsQP, Format::EVALUATION, true);
        auto sNew0 = sNewIn.GetElementAtIndex(0);
        sNew0.SetFormat(Format::COEFFICIENT);
        for (uint32_t i = 0; i < sizeQP; ++i) {
            if (i < sizeQ) { auto tmp = sNewIn.GetElementAtIndex(i); tmp.SetFormat(Format::EVALUATION); sNewExt.SetElementAtIndex(i, std::move(tmp)); }
            else { auto tmp = sNew0; tmp.SwitchModulus(pparamsQP[i]->GetModulus(), pparamsQP[i]->GetRootOfUnity(), 0, 0); tmp.SetFormat(Format::EVALUATION); sNewExt.SetElementAtIndex(i, std::move(tmp)); }
        }
        const uint32_t numPerPartQ = cryptoParams->GetNumPerPartQ();
        const auto& PModq          = cryptoParams->GetPModq();
        const auto& av = libKey->GetAVector();
        const auto& bv = libKey->GetBVector();
        double worstLog2 = -INFINITY;
        for (size_t part = 0; part < av.size(); ++part) {
            const uint32_t startPartIdx = numPerPartQ * part;
            const uint32_t endPartIdx   = (sizeQ > (startPartIdx + numPerPartQ)) ? (startPartIdx + numPerPartQ) : sizeQ;
            for (uint32_t i = 0; i < sizeQP; ++i) {
                const auto& ai = av[part].GetElementAtIndex(i);
                const auto& sni = sNewExt.GetElementAtIndex(i);
                NativePoly mine = (-ai * sni);
                if (i >= startPartIdx && i < endPartIdx)
                    mine += (PModq[i] * sOld.GetElementAtIndex(i));
                NativePoly diff = bv[part].GetElementAtIndex(i) - mine;  // should be ns*e_lib
                diff.SetFormat(Format::COEFFICIENT);
                const uint64_t q = diff.GetModulus().ConvertToInt();
                for (uint32_t j = 0; j < diff.GetLength(); ++j) {
                    uint64_t v = diff[j].ConvertToInt();
                    double c  = (v > q / 2) ? -static_cast<double>(q - v) : static_cast<double>(v);
                    worstLog2 = std::max(worstLog2, std::log2(std::fabs(c) + 1));
                }
            }
        }
        std::cout << "   formula " << label << ": max centered |b_lib - b_formula| ~ 2^" << std::fixed
                  << std::setprecision(1) << worstLog2 << std::defaultfloat
                  << (worstLog2 < 20 ? "  (noise-sized: formula matches)" : "  (NOT noise: formula or direction mismatch)")
                  << std::setprecision(6) << std::endl;
    };
    formulaCheck("relin (s^2 -> s)     ", libRelin[0], s * s, s);
    for (int32_t r : rotations) {
        uint32_t g = cc->FindAutomorphismIndex(r);
        formulaCheck("rot" + std::to_string(r) + " (s -> s(X^g^-1))", libRotMap->at(g), s, RotationTargetSecret(s, g));
    }

    // ---------------------------------------------------------------- 3. ship b halves + seed; server re-expands
    bool identity = true;
    {
        DCRTPoly again = SeededUniform(paramsPK, seed, 0);
        bool same      = (again == pkA);
        identity &= same;
        std::cout << "   pk      expanded a: " << (same ? "IDENTICAL" : "DIFFERENT") << "  checksum " << std::hex
                  << Checksum(again) << std::dec << std::endl;
    }
    auto srvRelin = ExpandKeySwitchKey(cc, appRelin->GetBVector(), keyTag, seed, 1);
    {
        bool same = SameVectors(srvRelin->GetAVector(), relinA);
        identity &= same;
        std::cout << "   relin   expanded a: " << (same ? "IDENTICAL" : "DIFFERENT") << "  checksum " << std::hex
                  << Checksum(srvRelin->GetAVector()[0]) << std::dec << std::endl;
    }
    auto srvRot = std::make_shared<std::map<uint32_t, EvalKey<DCRTPoly>>>();
    for (auto& [g, ek] : appRot) {
        (*srvRot)[g] = ExpandKeySwitchKey(cc, ek->GetBVector(), keyTag, seed, 1000 + 64ULL * g);
        bool same    = SameVectors((*srvRot)[g]->GetAVector(), rotA[g]);
        identity &= same;
        std::cout << "   rot " << std::setw(4) << rotOf[g] << " expanded a: " << (same ? "IDENTICAL" : "DIFFERENT")
                  << "  checksum " << std::hex << Checksum((*srvRot)[g]->GetAVector()[0]) << std::dec << std::endl;
    }

    // ---------------------------------------------------------------- 4. functional check
    std::vector<double> x(slots);
    for (uint32_t j = 0; j < slots; ++j)
        x[j] = std::sin(0.001 * j) * 0.9;
    auto pt = cc->MakeCKKSPackedPlaintext(x);

    // One op at a time inside a try/catch, so a bad key names the op instead of
    // aborting the lab (the skill's own debugging rule: localize, then classify).
    auto run = [&](const Ciphertext<DCRTPoly>& ct, const std::string& label) {
        double worst = 0;
        std::string failed;
        auto check = [&](const std::string& op, const Ciphertext<DCRTPoly>& c, const std::vector<double>& want) {
            try {
                Plaintext out;
                cc->Decrypt(keys.secretKey, c, &out);
                out->SetLength(slots);
                worst = std::max(worst, MaxAbsErr(out->GetRealPackedValue(), want, slots));
            }
            catch (const std::exception& ex) {
                worst = INFINITY;
                failed += (failed.empty() ? "" : ",") + op;
            }
        };
        for (int32_t r : rotations) {
            std::vector<double> want(slots);
            for (uint32_t j = 0; j < slots; ++j)
                want[j] = x[(j + slots + r) % slots];
            check("rot" + std::to_string(r), cc->EvalRotate(ct, r), want);
        }
        {
            std::vector<double> want(slots);
            for (uint32_t j = 0; j < slots; ++j)
                want[j] = x[j] * x[j];
            check("relin", cc->EvalMult(ct, ct), want);
        }
        std::cout << "   " << label << ": max |err| " << std::scientific << std::setprecision(2) << worst
                  << std::defaultfloat << std::setprecision(1) << std::fixed << "  (2^" << std::log2(worst) << ")"
                  << std::defaultfloat << std::setprecision(6) << (failed.empty() ? "" : "  DECODE FAIL at " + failed)
                  << std::endl;
        return worst;
    };

    auto ctLib = cc->Encrypt(keys.publicKey, pt);
    auto ctApp = cc->Encrypt(appPk, pt);

    // (a) library keys installed
    double eLibLib = run(ctLib, "lib ct / lib keys ");
    double eAppLib = run(ctApp, "app ct / lib keys ");

    // (b) server-expanded application keys installed instead
    CryptoContextImpl<DCRTPoly>::ClearEvalMultKeys();
    CryptoContextImpl<DCRTPoly>::ClearEvalAutomorphismKeys();
    CryptoContextImpl<DCRTPoly>::InsertEvalMultKey({srvRelin}, keyTag);
    CryptoContextImpl<DCRTPoly>::InsertEvalAutomorphismKey(srvRot, keyTag);
    double eLibApp = run(ctLib, "lib ct / app keys ");
    double eAppApp = run(ctApp, "app ct / app keys ");

    // restore (not needed for the lab, but keeps the context sane)
    CryptoContextImpl<DCRTPoly>::ClearEvalMultKeys();
    CryptoContextImpl<DCRTPoly>::ClearEvalAutomorphismKeys();
    CryptoContextImpl<DCRTPoly>::InsertEvalMultKey(libRelin, keyTag);
    CryptoContextImpl<DCRTPoly>::InsertEvalAutomorphismKey(libRotMap, keyTag);

    // ---------------------------------------------------------------- 5. sizes
    size_t fullRelin   = SerializedBytes(appRelin);
    size_t seededRelin = SerializedBytes(appRelin->GetBVector()) + 64 + 8;
    size_t fullPk      = SerializedBytes(keys.publicKey);
    size_t seededPk    = SerializedBytes(pkB) + 64 + 8;
    size_t fullRot = 0, seededRot = 0;
    for (auto& [g, ek] : appRot) {
        fullRot += SerializedBytes(ek);
        seededRot += SerializedBytes(ek->GetBVector()) + 64 + 8;
    }
    auto mib = [](size_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); };
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "   size pk     : full " << mib(fullPk) << " MiB  seeded " << mib(seededPk) << " MiB  ("
              << 100.0 * seededPk / fullPk << "%)" << std::endl;
    std::cout << "   size relin  : full " << mib(fullRelin) << " MiB  seeded " << mib(seededRelin) << " MiB  ("
              << 100.0 * seededRelin / fullRelin << "%)" << std::endl;
    std::cout << "   size rot x" << appRot.size() << ": full " << mib(fullRot) << " MiB  seeded " << mib(seededRot)
              << " MiB  (" << 100.0 * seededRot / fullRot << "%)" << std::endl;

    // ---------------------------------------------------------------- verdict
    double floor = std::max(eLibLib, 1e-12);
    bool errOk   = eAppApp <= 8 * floor && eLibApp <= 8 * floor && eAppLib <= 8 * floor;
    bool pass    = identity && errOk;
    std::cout << (pass ? "SEEDKEY PASS" : "SEEDKEY FAIL") << "  (identity " << (identity ? "ok" : "BROKEN")
              << ", app-key error within 8x of library floor " << (errOk ? "ok" : "NO") << ")" << std::endl;
    return pass ? 0 : 1;
}
