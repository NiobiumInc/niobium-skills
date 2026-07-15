#!/usr/bin/env bash
# fhe-gpu-smoke-test — verifies the GPU environment end to end.
# Requires a GPU at runtime (docker run --gpus all). Ends with "GPU SMOKE OK".
set -e

echo "[1/3] CUDA device visibility"
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader

echo "[2/3] FIDESlib device probe"
if command -v fideslib-gpu-test >/dev/null; then fideslib-gpu-test; else echo "(gpu-test binary not bundled; skipping)"; fi

echo "[3/3] compile + run a minimal FIDESlib CKKS program"
tmp=$(mktemp -d)
cat > "$tmp/smoke.cpp" <<'EOF'
#include <fideslib.hpp>
#include <iostream>
using namespace fideslib;
int main() {
    CCParams<CryptoContextCKKSRNS> p;
    p.SetMultiplicativeDepth(2);
    p.SetScalingModSize(50);
    p.SetBatchSize(8);
    p.SetDevices({0});
    p.SetCiphertextAutoload(true);
    p.SetPlaintextAutoload(false);
    auto cc = GenCryptoContext(p);
    cc->Enable(PKE); cc->Enable(KEYSWITCH); cc->Enable(LEVELEDSHE);
    auto keys = cc->KeyGen();
    cc->EvalMultKeyGen(keys.secretKey);
    cc->LoadContext(keys.publicKey);
    std::vector<double> x = {1, 2, 3, 4, 5, 6, 7, 8};
    // FIDESlib (like OpenFHE >=1.5) takes Plaintext by non-const ref: name it
    auto pt = cc->MakeCKKSPackedPlaintext(x);
    auto ct = cc->Encrypt(keys.publicKey, pt);
    auto sq = cc->EvalMult(ct, ct);
    Plaintext out; cc->Decrypt(keys.secretKey, sq, &out); out->SetLength(8);
    auto v = out->GetRealPackedValue();
    for (int i = 0; i < 8; ++i)
        if (std::abs(v[i] - double(x[i] * x[i])) > 1e-3) { std::cerr << "MISMATCH\n"; return 1; }
    std::cout << "square-on-GPU matches\n";
    return 0;
}
EOF
mkdir -p "$tmp/b"
cat > "$tmp/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.25.2)
set(CMAKE_CXX_STANDARD 20)
project(smoke LANGUAGES CXX)
find_package(fideslib REQUIRED CONFIG)
find_package(OpenMP REQUIRED)  # static OpenFHE/FIDESlib libs need gomp at link
set(CMAKE_CXX_COMPILER ${FIDESLIB_CXX_COMPILER})
add_executable(smoke smoke.cpp)
target_link_libraries(smoke PRIVATE fideslib::fideslib OpenMP::OpenMP_CXX)
EOF
cmake -S "$tmp" -B "$tmp/b" >/dev/null && cmake --build "$tmp/b" -j >/dev/null
"$tmp/b/smoke"
rm -rf "$tmp"
echo "GPU SMOKE OK"
