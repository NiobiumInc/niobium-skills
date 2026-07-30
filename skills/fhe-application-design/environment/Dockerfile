# syntax=docker/dockerfile:1
#
# ghcr.io/niobiuminc/fhe-dev — the FHE build-and-run environment for the
# fhe-application-design skill (Stage 0).
#
# Builds Niobium's instrumented OpenFHE fork + libnbfhetch from niobium-client
# and installs the SDK, so the four-program app builds against it and, run under
# a niobium::compiler() session, generates the FHETCH trace for the Fog. The twin
# and any PyTorch reference also run here (numpy / scipy / scikit-learn / CPU
# torch). The bootstrap parameter lab is baked in as `fhe-boot-lab` (Stage 6).
#
# This Dockerfile lives in the skill and builds the image directly. HAZE (the
# GPU C API) is intentionally omitted.
#
# Build (from this directory):
#   docker build -t ghcr.io/niobiuminc/fhe-dev:v0.13.0 .
#   docker build --build-arg GIT_REF=<sha|branch|tag> -t ghcr.io/niobiuminc/fhe-dev:v0.13.0 .
#   docker build --build-arg BUILD_JOBS=4 -t ghcr.io/niobiuminc/fhe-dev:v0.13.0 .   # more RAM
#
# Run (project bind-mounted at /work for app builds):
#   docker run --rm ghcr.io/niobiuminc/fhe-dev:v0.13.0 make test-release        # smoke test
#   docker run --rm -v "$PWD":/work -w /work ghcr.io/niobiuminc/fhe-dev:v0.13.0 <cmd>

ARG UBUNTU_VERSION=24.04
FROM ubuntu:${UBUNTU_VERSION} AS base

# Toolchain + TLS backend. MATHBACKEND=4 with NTL/TCM/OPENMP/NATIVEOPT=OFF keeps
# the dependency graph tiny. git fetches the submodules; libssl-dev compiles the
# FHETCH transport client with TLS so it accepts https:// worker URLs (the Fog
# path); ca-certificates verifies those certs and lets the in-image git clone
# reach GitHub. python3 backs the twin, the references, and the nb DSL.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates libssl-dev \
        python3 python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Python stack for the twin tier (Stages 1-7) and PyTorch references (Stage 3).
RUN pip3 install --no-cache-dir --break-system-packages numpy scipy scikit-learn \
 && pip3 install --no-cache-dir --break-system-packages torch --index-url https://download.pytorch.org/whl/cpu

# Clone niobium-client and init only niobium-fhetch (+ its nested instrumented
# OpenFHE fork + json). niobium-haze (the GPU C API) is skipped: this SDK does
# not ship it and the build never references it.
ARG GIT_REPO=https://github.com/NiobiumInc/niobium-client.git
ARG GIT_REF=main
WORKDIR /opt
RUN git clone "${GIT_REPO}" niobium-client \
    && git -C niobium-client checkout "${GIT_REF}" \
    && git -C niobium-client submodule update --init --recursive vendor/niobium-fhetch

WORKDIR /opt/niobium-client

# Cap build parallelism by MEMORY, not cores: heavy OpenFHE/template TUs use
# ~2-3 GB each in cc1plus. Override to match host RAM: --build-arg BUILD_JOBS=N.
ARG BUILD_JOBS=2
ENV CMAKE_BUILD_PARALLEL_LEVEL=${BUILD_JOBS}

# Build (Release) and install the SDK into the image. Examples stay OFF; the
# transport tools build. The install ships NiobiumFhetchConfig.cmake, so an app
# builds against it with find_package(NiobiumFhetch) alone. install-cli puts the
# `fog` CLI + nbcc_fhetch_replay on PATH (CLI_PREFIX=/usr/local) for the Fog path.
RUN make config-release build-release \
    && make install-release \
    && make install-cli CLI_PREFIX=/usr/local

# Bootstrap parameter lab (Stage 6): measures REAL CKKS bootstrap accuracy at
# candidate parameters in minutes (EvalBootstrap silently no-ops on shallow
# inputs, so it burns inputs down first). Compiled against the installed OpenFHE.
COPY boot_lab.cpp /tmp/boot_lab.cpp
# The instrumented OpenFHE fork references openfhe_cprobe_* hooks that live in
# libnbfhetch, so even a pure-OpenFHE program must link nbfhetch to resolve them.
RUN OFHE=/opt/niobium-client/vendor/lib/openfhe \
 && NC=/opt/niobium-client/vendor/lib/niobium-client \
 && g++ -O2 -std=c++17 /tmp/boot_lab.cpp -o /usr/local/bin/fhe-boot-lab \
        -I"$OFHE/include/openfhe" \
        -I"$OFHE/include/openfhe/core" \
        -I"$OFHE/include/openfhe/pke" \
        -I"$OFHE/include/openfhe/binfhe" \
        -I"$OFHE/include/openfhe/third-party/include" \
        -L"$OFHE/lib" -lOPENFHEpke -lOPENFHEcore -lOPENFHEbinfhe \
        -L"$NC/lib" -lnbfhetch \
        -Wl,-rpath,"$OFHE/lib" -Wl,-rpath,"$NC/lib" \
 && rm /tmp/boot_lab.cpp

# Interactive conveniences: an in-container editor, and git-lfs so bind-mounted
# project repos can fetch their LFS-tracked inputs (datasets, model blobs) from
# inside the container. Kept in a late layer so it doesn't invalidate the
# OpenFHE build cache above.
RUN apt-get update && apt-get install -y --no-install-recommends vim git-lfs \
    && git lfs install --system \
    && rm -rf /var/lib/apt/lists/*

# Default to the SDK source tree so `make test-release` and the examples work
# out of the box. App builds bind-mount the project and run with `-w /work`.
WORKDIR /opt/niobium-client
CMD ["/bin/bash"]
