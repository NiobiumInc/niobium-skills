// fhe-cheb-lab — polynomial-approximation precision probe (baked into fhe-dev).
//
// Answers ONE question empirically, in seconds: "when I evaluate this
// polynomial under CKKS at these parameters, how much of the error is the
// polynomial not being the function, and how much is the encryption?" The two
// have different fixes (a better fit vs. a larger scaling modulus or smaller
// coefficients), and a single end-to-end error number cannot tell them apart.
//
// It reports three numbers over a dense grid on the fit domain (and over your
// own operand samples, if given):
//   bias   = max |p(x) - f(x)|      the approximation error, computed in double
//   noise  = max |Dec(p(Enc x)) - p(x)|   the CKKS evaluation error of THIS series
//   total  = max |Dec(p(Enc x)) - f(x)|   what the application sees
// plus where the noise peaks (it grows toward the domain edges, where the
// Chebyshev basis is largest), the coefficient 1-norm (the noise amplifier),
// the levels the evaluation consumed, and optionally a sweep over scaling
// sizes, which shows noise falling by ~1 bit per scaling bit while bias stays
// fixed — the signature that separates the two.
//
// Usage:
//   fhe-cheb-lab <func|coeffs:FILE> <a> <b> <degree> [scaling=45] [first=55]
//                [depth=0(auto)] [ringDim=16384] [secure=0] [samples=FILE]
//                [sweep=0]
//   func:   sigmoid tanh gelu relu sign step inv rsqrt exp log softplus silu
//   coeffs: a text file with one Chebyshev coefficient per line in OpenFHE's
//           EvalChebyshevSeries convention (c0/2 + sum c_k T_k on [a,b]) —
//           e.g. the output of scripts/cheb_fit.py --emit
//   samples: one operand value per line; reported separately from the grid
//   sweep=1: repeat at scaling 35,40,45,50,55 (first = scaling+10, <= 60)
// Examples:
//   fhe-cheb-lab sigmoid -8 8 27                       # OpenFHE's own fit
//   fhe-cheb-lab coeffs:sigmoid_wls.txt -8 8 27 45 55 0 16384 0 operands.txt
//   fhe-cheb-lab relu -1 1 31 45 55 0 16384 0 "" 1    # scaling sweep
// Interpretation:
//   - total ≈ bias and noise ≪ bias: the fit is the problem (degree, domain,
//     method, or the target function itself); encryption is not.
//   - noise ≈ total and bias ≪ noise: raise the scaling modulus or choose a
//     fit with a smaller coefficient 1-norm; more degree will not help.
//   - noise that does not fall with scaling: the series overflowed the scale
//     (coefficients or operands too large); check the domain and the norm.

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "openfhe.h"
#include "math/chebyshev.h"  // EvalChebyshevCoefficients (core)

using namespace lbcrypto;

namespace {

std::map<std::string, std::function<double(double)>> Presets() {
    return {
        {"sigmoid", [](double x) { return 1.0 / (1.0 + std::exp(-x)); }},
        {"tanh", [](double x) { return std::tanh(x); }},
        {"gelu", [](double x) { return 0.5 * x * (1.0 + std::erf(x / std::sqrt(2.0))); }},
        {"relu", [](double x) { return x > 0 ? x : 0.0; }},
        {"sign", [](double x) { return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0); }},
        {"step", [](double x) { return x > 0 ? 1.0 : 0.0; }},
        {"inv", [](double x) { return 1.0 / x; }},
        {"rsqrt", [](double x) { return 1.0 / std::sqrt(x); }},
        {"exp", [](double x) { return std::exp(x); }},
        {"log", [](double x) { return std::log(x); }},
        {"softplus", [](double x) { return std::log1p(std::exp(x)); }},
        {"silu", [](double x) { return x / (1.0 + std::exp(-x)); }},
    };
}

// Clenshaw evaluation of c0/2 + sum_{k>=1} c_k T_k(t), t = (2x-a-b)/(b-a):
// the exact convention of OpenFHE's EvalChebyshevSeries.
double EvalSeriesPlain(const std::vector<double>& c, double a, double b, double x) {
    double t  = (2.0 * x - a - b) / (b - a);
    double b1 = 0, b2 = 0;
    for (size_t k = c.size() - 1; k >= 1; --k) {
        double b0 = 2.0 * t * b1 - b2 + c[k];
        b2        = b1;
        b1        = b0;
    }
    return t * b1 - b2 + 0.5 * c[0];
}

std::vector<double> ReadDoubles(const std::string& path) {
    std::vector<double> v;
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    double d;
    while (in >> d)
        v.push_back(d);
    return v;
}

struct Report {
    double bias = 0, noise = 0, total = 0, rmsBias = 0, rmsNoise = 0, xAtMaxNoise = 0;
};

Report Compare(const std::vector<double>& xs, const std::vector<double>& dec, const std::vector<double>& c,
               double a, double b, const std::function<double(double)>& f, bool haveF) {
    Report r;
    double sb = 0, sn = 0;
    for (size_t i = 0; i < xs.size(); ++i) {
        double p  = EvalSeriesPlain(c, a, b, xs[i]);
        double n  = std::fabs(dec[i] - p);
        sn += n * n;
        if (n > r.noise) {
            r.noise       = n;
            r.xAtMaxNoise = xs[i];
        }
        if (haveF) {
            double fx = f(xs[i]);
            double e  = std::fabs(p - fx);
            sb += e * e;
            r.bias  = std::max(r.bias, e);
            r.total = std::max(r.total, std::fabs(dec[i] - fx));
        }
    }
    r.rmsBias  = std::sqrt(sb / xs.size());
    r.rmsNoise = std::sqrt(sn / xs.size());
    return r;
}

double Log2(double v) {
    return v > 0 ? std::log2(v) : -INFINITY;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "usage: fhe-cheb-lab <func|coeffs:FILE> <a> <b> <degree> [scaling=45] [first=55] [depth=0] "
                     "[ringDim=16384] [secure=0] [samples=FILE] [sweep=0]"
                  << std::endl;
        return 2;
    }
    std::string spec = argv[1];
    double a         = atof(argv[2]);
    double b         = atof(argv[3]);
    uint32_t degree  = atoi(argv[4]);
    uint32_t scaling = argc > 5 ? atoi(argv[5]) : 45;
    uint32_t first   = argc > 6 ? atoi(argv[6]) : 55;
    uint32_t depthIn = argc > 7 ? atoi(argv[7]) : 0;
    uint32_t ringDim = argc > 8 ? atoi(argv[8]) : 16384;
    bool secure      = argc > 9 && atoi(argv[9]) != 0;
    std::string samplesPath = argc > 10 ? argv[10] : "";
    bool sweep       = argc > 11 && atoi(argv[11]) != 0;

    // --- the function and the series
    auto presets = Presets();
    std::function<double(double)> f;
    bool haveF = false;
    std::vector<double> coeffs;
    if (spec.rfind("coeffs:", 0) == 0) {
        coeffs = ReadDoubles(spec.substr(7));
        if (coeffs.size() != degree + 1)
            std::cerr << "   note: " << coeffs.size() << " coefficients read; using their degree "
                      << coeffs.size() - 1 << " in place of " << degree << std::endl;
        degree = coeffs.size() - 1;
        // a preset name after a second colon lets the lab score bias too: coeffs:FILE:sigmoid
    }
    else {
        auto it = presets.find(spec);
        if (it == presets.end()) {
            std::cerr << "unknown function " << spec << std::endl;
            return 2;
        }
        f      = it->second;
        haveF  = true;
        coeffs = EvalChebyshevCoefficients(f, a, b, degree);  // OpenFHE's own interpolation
    }
    {
        // coeffs:FILE:func form
        size_t p2 = spec.find(':', 7);
        if (spec.rfind("coeffs:", 0) == 0 && p2 != std::string::npos) {
            auto it = presets.find(spec.substr(p2 + 1));
            if (it != presets.end()) {
                f     = it->second;
                haveF = true;
            }
        }
    }
    double norm1 = 0, normMax = 0;
    for (double c : coeffs) {
        norm1 += std::fabs(c);
        normMax = std::max(normMax, std::fabs(c));
    }
    const uint32_t depthSeries = static_cast<uint32_t>(std::ceil(std::log2(degree + 1.0))) + 1;
    const uint32_t depth       = depthIn ? depthIn : depthSeries + 1;

    std::cout << "== fhe-cheb-lab: " << spec << " on [" << a << ", " << b << "]  degree " << degree
              << "  (series depth " << depthSeries << ", context depth " << depth << ")  ring " << ringDim
              << (secure ? "  HEStd_128_classic" : "  HEStd_NotSet (toy)") << " ==" << std::endl;
    std::cout << std::scientific << std::setprecision(2);
    std::cout << "   coefficients: 1-norm " << norm1 << "  (2^" << std::fixed << std::setprecision(1) << Log2(norm1)
              << ")  max |c_k| " << std::scientific << std::setprecision(2) << normMax << std::endl;

    // --- the operands: a dense grid over [a,b] in the slots, and optional samples
    const uint32_t slots = ringDim / 2;
    std::vector<double> grid(slots);
    for (uint32_t j = 0; j < slots; ++j)
        grid[j] = a + (b - a) * (j + 0.5) / slots;
    std::vector<double> samples;
    if (!samplesPath.empty()) {
        samples = ReadDoubles(samplesPath);
        if (samples.size() > slots)
            samples.resize(slots);
        for (double& s : samples)
            s = std::min(b, std::max(a, s));  // the series is only defined on [a,b]
    }

    auto runOnce = [&](uint32_t sc, uint32_t fm) {
        CCParams<CryptoContextCKKSRNS> p;
        p.SetMultiplicativeDepth(depth);
        p.SetScalingModSize(sc);
        p.SetFirstModSize(fm);
        p.SetScalingTechnique(FLEXIBLEAUTO);
        p.SetRingDim(ringDim);
        p.SetBatchSize(slots);
        p.SetSecurityLevel(secure ? HEStd_128_classic : HEStd_NotSet);
        auto cc = GenCryptoContext(p);
        cc->Enable(PKE);
        cc->Enable(KEYSWITCH);
        cc->Enable(LEVELEDSHE);
        cc->Enable(ADVANCEDSHE);
        auto keys = cc->KeyGen();
        cc->EvalMultKeyGen(keys.secretKey);

        auto evalOn = [&](const std::vector<double>& xs, const std::string& label) {
            auto pt = cc->MakeCKKSPackedPlaintext(xs);
            auto ct = cc->Encrypt(keys.publicKey, pt);
            auto t0 = std::chrono::steady_clock::now();
            auto out = cc->EvalChebyshevSeries(ct, coeffs, a, b);
            auto t1  = std::chrono::steady_clock::now();
            Plaintext dec;
            try {
                cc->Decrypt(keys.secretKey, out, &dec);
            }
            catch (const std::exception& ex) {
                std::cout << "   " << label << ": DECODE FAIL (" << ex.what() << ")" << std::endl;
                return Report{INFINITY, INFINITY, INFINITY, INFINITY, INFINITY, 0};
            }
            dec->SetLength(xs.size());
            auto r = Compare(xs, dec->GetRealPackedValue(), coeffs, a, b, f, haveF);
            // Effective levels: GetLevel() plus any rescale FLEXIBLEAUTO left pending
            // (noiseScaleDeg 2 means one more rescale is owed), so the count matches
            // what the next multiply will actually see.
            const int64_t usedEff = (static_cast<int64_t>(out->GetLevel()) + out->GetNoiseScaleDeg() - 1) -
                                    (static_cast<int64_t>(ct->GetLevel()) + ct->GetNoiseScaleDeg() - 1);
            std::cout << "   " << label << " @ scaling " << sc << ": levels used " << usedEff << " of " << depth
                      << " (charged " << depthSeries << ")"
                      << ", " << std::fixed << std::setprecision(2)
                      << std::chrono::duration<double>(t1 - t0).count() << " s" << std::endl;
            std::cout << std::scientific << std::setprecision(2);
            if (haveF)
                std::cout << "      bias  max " << r.bias << " (2^" << std::fixed << std::setprecision(1) << Log2(r.bias)
                          << ")  rms " << std::scientific << std::setprecision(2) << r.rmsBias << std::endl;
            std::cout << "      noise max " << r.noise << " (2^" << std::fixed << std::setprecision(1) << Log2(r.noise)
                      << ")  rms " << std::scientific << std::setprecision(2) << r.rmsNoise << "  at x = " << std::fixed
                      << std::setprecision(3) << r.xAtMaxNoise << std::endl;
            if (haveF)
                std::cout << "      total max " << std::scientific << std::setprecision(2) << r.total << " (2^"
                          << std::fixed << std::setprecision(1) << Log2(r.total) << ")"
                          << (r.bias > 4 * r.noise  ? "   <- the fit dominates"
                              : r.noise > 4 * r.bias ? "   <- encryption dominates"
                                                     : "   <- comparable")
                          << std::endl;
            return r;
        };
        Report g = evalOn(grid, "grid   ");
        if (!samples.empty())
            evalOn(samples, "samples");
        return g;
    };

    if (!sweep) {
        runOnce(scaling, first);
    }
    else {
        std::cout << "   scaling sweep (bias is fixed by the fit; noise should fall ~1 bit per scaling bit):" << std::endl;
        for (uint32_t sc : {35u, 40u, 45u, 50u, 55u})
            runOnce(sc, std::min(60u, sc + 10));
    }
    return 0;
}
