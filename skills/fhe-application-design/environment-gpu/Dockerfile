# FHE-dev-GPU — CUDA build-and-run environment for the GPU server tier.
#
# Ships FIDESlib (server-side CKKS on NVIDIA GPUs, OpenFHE-interoperable) plus
# the PATCHED OpenFHE fork FIDESlib requires (installed automatically via
# FIDESLIB_INSTALL_OPENFHE=ON). Client programs (keygen/encrypt/decrypt) keep
# using the CPU image; only the server stage runs here.
#
# Target hardware: NVIDIA RTX 5090 (Blackwell, sm_120) — hence CUDA >= 12.8.
# Override FIDESLIB_ARCH for other cards (e.g. 89 = Ada, 90 = Hopper), or
# "all-major" for a fat (slow-to-build) image.
#
# Build (no GPU needed to compile):
#   docker build -t ghcr.io/niobiuminc/fhe-dev-gpu:dev .
# Run (GPU required):
#   docker run --rm --gpus all -v "$PWD":/work -w /work ghcr.io/niobiuminc/fhe-dev-gpu:dev <cmd>

FROM docker.io/nvidia/cuda:12.8.1-devel-ubuntu22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      git \
      ca-certificates \
      libomp-dev \
      python3 \
      python3-pip \
    && rm -rf /var/lib/apt/lists/*

# FIDESlib needs CMake >= 3.25.2; Ubuntu 22.04 ships 3.22 — take it from pip.
RUN pip3 install --no-cache-dir "cmake>=3.25.2,<4"

# --- FIDESlib + patched OpenFHE (pinned) --------------------------------------
# FIDESLIB_INSTALL_OPENFHE=ON pulls and installs FIDESlib's patched OpenFHE
# fork (fideslib-ref-v1.5.1.x — same v1.5.1 wire format as fhe-dev >= v0.7.0,
# so ciphertexts/keys serialized by the CPU client load here).
# NOTE: pin FIDESLIB_VERSION to a release tag once validated on hardware.
ARG FIDESLIB_VERSION=main
ARG FIDESLIB_ARCH=120
RUN git clone --depth 1 --branch ${FIDESLIB_VERSION} \
        https://github.com/CAPS-UMU/FIDESlib.git /tmp/fideslib \
 && cmake -S /tmp/fideslib -B /tmp/fideslib/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DFIDESLIB_ARCH=${FIDESLIB_ARCH} \
        -DFIDESLIB_INSTALL_OPENFHE=ON \
        -DFIDESLIB_COMPILE_TESTS=OFF \
        -DFIDESLIB_COMPILE_BENCHMARKS=OFF \
 && cmake --build /tmp/fideslib/build -j"$(nproc)" \
 && cmake --build /tmp/fideslib/build --target install -j \
 && cp /tmp/fideslib/build/gpu-test /usr/local/bin/fideslib-gpu-test 2>/dev/null || true \
 && ldconfig \
 && rm -rf /tmp/fideslib

COPY smoke_test_gpu.sh /usr/local/bin/fhe-gpu-smoke-test
RUN chmod +x /usr/local/bin/fhe-gpu-smoke-test

WORKDIR /work
CMD ["/bin/bash"]
