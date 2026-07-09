# FHE-dev — the build-and-run environment for the fhe-application-design skill.
#
# Ships everything the skill's Stage 8 needs to BUILD a user's own four-program
# OpenFHE application, plus a Python ML stack so references and twins that need
# torch/sklearn can run here when Claude's own sandbox can't fit them:
#
#   - OpenFHE (C++ dev: headers + libs + CMake package) — build new FHE code
#   - CMake, a C++20 compiler, git
#   - Python 3 + numpy / scipy / scikit-learn / torch (CPU)
#
# Distribution model: this image is BUILT ONCE and published to GHCR. Users
# `docker pull` it — they never build OpenFHE on their own machine. See
# ../references/environment-setup.md for how it is used.
#
# Build + publish (maintainers):
#   docker build -t ghcr.io/niobiuminc/fhe-dev:latest .
#   docker push ghcr.io/niobiuminc/fhe-dev:latest

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      git \
      ca-certificates \
      libomp-dev \
      python3 \
      python3-pip \
    && rm -rf /var/lib/apt/lists/*

# --- OpenFHE (pinned release) -----------------------------------------------
# Build once, install to /usr/local, then drop the source tree to keep the
# image smaller. This is the slow layer; it only runs at image-build time.
ARG OPENFHE_VERSION=v1.2.3
RUN git clone --depth 1 --branch ${OPENFHE_VERSION} \
        https://github.com/openfheorg/openfhe-development.git /tmp/openfhe \
 && cmake -S /tmp/openfhe -B /tmp/openfhe/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_UNITTESTS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_BENCHMARKS=OFF \
        -DWITH_OPENMP=ON \
 && cmake --build /tmp/openfhe/build -j"$(nproc)" \
 && cmake --install /tmp/openfhe/build \
 && ldconfig \
 && rm -rf /tmp/openfhe

# --- Python ML stack (references + faithful twins) --------------------------
# torch is CPU-only to keep the image portable; it is the heavy layer. If you
# don't need torch references, a slimmer tag can drop it.
RUN pip3 install --no-cache-dir --upgrade pip \
 && pip3 install --no-cache-dir numpy scipy scikit-learn \
 && pip3 install --no-cache-dir torch --index-url https://download.pytorch.org/whl/cpu

# --- Smoke test -------------------------------------------------------------
COPY smoke_test.sh /usr/local/bin/fhe-smoke-test
RUN chmod +x /usr/local/bin/fhe-smoke-test

# The mounted project folder is bind-mounted here at run time:
#   docker run --rm -v "$PWD":/work ghcr.io/niobiuminc/fhe-dev:latest <cmd>
WORKDIR /work
CMD ["/bin/bash"]
