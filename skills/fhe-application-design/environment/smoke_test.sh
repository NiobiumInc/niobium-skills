#!/usr/bin/env bash
# fhe-smoke-test — prove the FHE-dev environment can BUILD and RUN OpenFHE C++
# and run a numpy twin, before any design work starts (skill Stage 0).
#
# Run it inside the container:
#   docker run --rm ghcr.io/niobiuminc/fhe-dev:latest fhe-smoke-test
#
# Exit 0 and a final "SMOKE OK" line mean the environment is good.
set -euo pipefail

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# 1) OpenFHE C++: compile and run a trivial CKKS EvalAdd.
cat > "$work/smoke.cpp" <<'CPP'
#include "openfhe.h"
using namespace lbcrypto;
int main() {
    CCParams<CryptoContextCKKSRNS> p;
    p.SetMultiplicativeDepth(1);
    p.SetScalingModSize(50);
    p.SetRingDim(1 << 14);
    auto cc = GenCryptoContext(p);
    cc->Enable(PKE);
    cc->Enable(LEVELEDSHE);
    auto keys = cc->KeyGen();
    // Explicit vector<double> — a bare {..} is ambiguous between the double and
    // complex<double> MakeCKKSPackedPlaintext overloads.
    std::vector<double> va{1.0, 2.0, 3.0};
    std::vector<double> vb{4.0, 5.0, 6.0};
    auto a = cc->MakeCKKSPackedPlaintext(va);
    auto b = cc->MakeCKKSPackedPlaintext(vb);
    auto ca = cc->Encrypt(keys.publicKey, a);
    auto cb = cc->Encrypt(keys.publicKey, b);
    auto cs = cc->EvalAdd(ca, cb);
    Plaintext r;
    cc->Decrypt(keys.secretKey, cs, &r);
    r->SetLength(3);
    std::cout << "OpenFHE EvalAdd -> " << r << std::endl;   // expect ~(5, 7, 9)
    return 0;
}
CPP

cat > "$work/CMakeLists.txt" <<'CM'
cmake_minimum_required(VERSION 3.16)
project(fhe_smoke CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(OpenFHE REQUIRED)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenFHE_CXX_FLAGS}")
add_executable(smoke smoke.cpp)
# OpenFHE splits its headers across core/, pke/, and binfhe/ (base-fhe.h pulls in
# binfhecontext.h), plus its bundled third-party headers. Include them all.
target_include_directories(smoke PRIVATE
    ${OpenFHE_INCLUDE}
    ${OpenFHE_INCLUDE}/third-party/include
    ${OpenFHE_INCLUDE}/core
    ${OpenFHE_INCLUDE}/pke
    ${OpenFHE_INCLUDE}/binfhe)
# Link via OpenFHE's own variable rather than naming targets (robust across releases).
target_link_libraries(smoke PRIVATE ${OpenFHE_SHARED_LIBRARIES})
CM

echo "[1/2] building + running a trivial OpenFHE CKKS program..."
cmake -S "$work" -B "$work/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$work/build" -j"$(nproc)" >/dev/null
"$work/build/smoke"

# 2) Python twin tier: numpy is present and works.
echo "[2/2] running a numpy twin stub..."
python3 -c "import numpy as np; print('numpy twin stub -> tanh([0,1,2]) =', np.round(np.tanh(np.array([0.,1.,2.])), 4))"

echo "SMOKE OK"
