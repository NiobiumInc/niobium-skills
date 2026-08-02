# Implementing the Design in OpenFHE C++

This reference covers the **Stage 8 OpenFHE implementation path**: the hand-written
OpenFHE build mechanics for the four-program application whose architecture,
deliverables, and validation are defined in SKILL.md Stage 8. Follow that
path-independent contract (the four-program trust split, `run_test`, the
client/server demo, client-side bounds enforcement, twin validation); this file is
the OpenFHE-specific how-to for the build itself.

Contents:
- [Build and link (CMake)](#build-and-link-cmake)
- [Context features and serialization](#context-features-and-serialization)
- [The shared run_circuit()](#the-shared-run_circuit)

## Build and link (CMake)

Build the four programs by linking the installed SDK through
`find_package(NiobiumFhetch)`. That single package pulls in `libnbfhetch`, the
instrumented OpenFHE headers and libraries, and the `openfhe_cprobe_*` hooks, so
there is no manual OpenFHE wiring. **Use this `CMakeLists.txt`** (verified in the
image):

```cmake
cmake_minimum_required(VERSION 3.16)
project(app CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(NiobiumFhetch REQUIRED)

add_executable(server server.cpp)      # likewise keygen / encrypt / decrypt
target_link_libraries(server PRIVATE Niobium::niobium_fhetch)
```

## Context features and serialization

The client programs (keygen/encrypt/decrypt) never open a session, but linking
them the same way is harmless. Enable PKE, KEYSWITCH, and LEVELEDSHE features on
the crypto context, plus ADVANCEDSHE for Chebyshev evaluation or advanced rotation
patterns. Use OpenFHE's serialization API (Serial::SerializeToFile /
Serial::DeserializeFromFile) for all inter-program data exchange.

(On a niobium-client older than the `find_package` Config, link the target the
manual way instead: `find_package(OpenFHE)` +
`include(<prefix>/lib/cmake/NiobiumFhetch/NiobiumFhetchTargets.cmake)` +
`target_link_directories(server PRIVATE ${OpenFHE_LIBDIR})`.)

## The shared run_circuit()

**Factor the circuit into a shared `run_circuit()`.** Put the homomorphic
circuit body in one function in `common.hpp`:

```cpp
// common.hpp: the circuit body, called by the server in both run modes
inline Ciphertext<DCRTPoly> run_circuit(
        CryptoContext<DCRTPoly>& cc, const Model& m,
        const std::vector<Ciphertext<DCRTPoly>>& x) {
    // ... the exact forward pass (Linear -> activation -> Linear -> ...) ...
    return result;
}
```

The server calls `run_circuit(...)` in every mode. On `--cpu` it serializes the
OpenFHE result directly; `--sim` and the default both wrap `run_circuit(...)` in a
`niobium::compiler()` session to generate the trace, then reconstruct the result
locally through `fhetch_sim` (`--sim`) or from the Fog (default, Stage 10).
Keeping the circuit in one place means the modes cannot diverge.

The `niobium::compiler()` session calls the `--sim` and default modes need are in
[niobium-client-fog-variant.md](niobium-client-fog-variant.md) under "The
recording pattern". Author the server with them now, so the one binary supports
all three modes.
