# Emitting a niobium-client (Fog) variant of a finished OpenFHE app

This is an **optional Stage 8 add-on**. It takes an application that already
works the normal way — the four OpenFHE programs (keygen / encrypt / server /
decrypt), `run_test` green against the faithful twin, and the two-process demo
standing — and produces a second build of the *same* application that runs
through the **Niobium Mistic / Fog** path: the computation is recorded as a
FHETCH Polynomial IR trace (`.fhetch`) via niobium-client's instrumented
OpenFHE, replayed in the local simulator for validation, and (optionally)
submitted to the Niobium compilation service for deployment to hardware.

## When to offer it

Only after the CPU app is **built and validated**. This is a deployment-target
variant of a known-good design, not a design path. Never design straight to the
Fog path, and never offer it before `run_test` passes. It is **opt-in**: present
the option to the user and emit it only if they choose it.

## Where it goes

A parallel directory in the **application repo**, alongside the CPU app:

```
fhe-design/
├── app/          # the validated four-program OpenFHE app (unchanged)
└── app-fog/      # the niobium-client variant (this add-on)
```

Never write into the niobium-client repository. `app-fog/` is emitted into the
application repo you are working in; niobium-client is only a build-time
dependency (see Build below). This mirrors the `app-gpu/` convention used for
the GPU/FIDESlib variant.

## The governing rule: instrument, don't redesign

The Fog server must run the **identical circuit** as the validated
`app/server.cpp` — same weights, same parameters, same packing. The only
additions are the `niobium::compiler()` session calls that bracket the
computation. To make that guarantee structural rather than a matter of
discipline, **factor the circuit body into a shared function** and have both
servers call it:

```cpp
// common.hpp (shared by app/ and app-fog/)
inline Ciphertext<DCRTPoly> run_circuit(
        CryptoContext<DCRTPoly>& cc, const Model& m,
        const std::vector<Ciphertext<DCRTPoly>>& x) {
    // ... the exact forward pass (Linear -> activation -> Linear -> ...) ...
    return result;
}
```

The plain server calls `run_circuit(...)` directly; the Fog server calls the
same function between `start()` and `stop()`. The math cannot drift because
there is only one copy of it. (This retires the divergent-parallel-source pain
seen in the GPU pilot.)

## Only the server is instrumented

The compute is the thing the Fog accelerates, so only the **server** is
instrumented. `keygen` / `encrypt` / `decrypt` are the client-side programs and
are reused essentially unchanged (packaged as the example's `client` and
`decrypt`, matching niobium-client's example triple convention).

## The recording pattern

The instrumented server follows niobium-client's example convention exactly:

```cpp
#include "openfhe.h"
#include "niobium/compiler.h"
// ... openfhe serialization headers, common.hpp ...

niobium::compiler().init(argc, argv);
niobium::compiler().set_program_info("<app>_server", "1.0", "<desc>");
niobium::compiler().set_build_info(__FILE__, __LINE__, __TIMESTAMP__);
niobium::Compiler::CacheParameters params;
params.push_back({"workload", "<app>"});
niobium::compiler().cache_parameters(params);

// load context, input ciphertexts, eval keys, model (as in app/server.cpp)
niobium::compiler().capture_crypto_context(cc);
for (each input ct) niobium::compiler().tag_input("<name>", ct);
niobium::compiler().tag_keys(cc);

if (!niobium::compiler().is_cache_valid()) {
    niobium::compiler().start();
    auto out = run_circuit(cc, model, inputs);     // the SHARED circuit
    niobium::compiler().probe("<output>", out);
    niobium::compiler().stop();                     // writes .fhetch + fhetch_replay.json
}
niobium::compiler().replay();                       // drives fhetch_sim in-process
Ciphertext<DCRTPoly> ct_result;
niobium::compiler().result(cc, "<output>", ct_result);
// serialize ct_result for decrypt
```

`result()` also lets you diff the simulator output against OpenFHE's at the ring
level — a free "bit-identical" check (see Verification).

## Build (approach A — proven default)

`app-fog/` links niobium-client's `niobium_fhetch` target, which lives in the
niobium-client build. The simplest supported path is to **graft `app-fog/` into
a niobium-client checkout** and build with its toolchain:

1. Copy or symlink `app-fog/` into `<niobium-client>/examples/diabetes/` (any
   example name), and add a `client`/`server`/`decrypt` triple to
   `examples/CMakeLists.txt`:
   ```cmake
   add_executable(<app>_client  <dir>/client.cpp)
   add_executable(<app>_server  <dir>/server.cpp)
   add_executable(<app>_decrypt <dir>/decrypt.cpp)
   foreach(p <app>_client <app>_server <app>_decrypt)
       target_link_libraries(${p} PRIVATE niobium_fhetch)
       set_openfhe_rpath(${p})
   endforeach()
   ```
2. Build the client + examples: `make sync` (first time) then `make build-release`.
3. Run from the niobium-client repo root:
   ```
   build/examples/<app>_client  <keys> <app-fog>/data
   build/examples/<app>_server  <keys> <app-fog>/data --no-ring-dim-check
   build/examples/<app>_decrypt <keys> <app-fog>/data
   ```

`--no-ring-dim-check` is required whenever N differs from niobium-client's toy
default (production designs use N = 2^16).

**Version parity:** niobium-client's instrumented OpenFHE must be the same
OpenFHE version the app targets (the FHE-dev image line, currently v1.5.1 /
OpenFHE 1.5.1). Confirm before building.

*(Approach B — a standalone `app-fog/CMakeLists.txt` that builds against an
installed niobium-client via `find_package`/`find_library` — is cleaner but
depends on niobium-client exposing an install/export target; use it only once
that is confirmed.)*

## Verification gate

The Fog variant must clear the **same** bar as the CPU app, plus a free extra:

- **Fog-replay vs the faithful twin**: max output error in the encryption-noise
  band and **0 decision flips** (identical criterion to `run_test`). The
  recorded design and parameters are unchanged, so this should hold.
- **Simulator vs OpenFHE (free)**: `result()` reconstructs the probe from the
  replayed trace; comparing it to OpenFHE's own computed ciphertext at the ring
  level should be **bit-identical**. A mismatch means the recorder/replayer did
  not reproduce the op stream — a trace problem, not a design problem.

## Caveats and notes

- **Local-sim cost.** Deep circuits (e.g. a high-degree Chebyshev over many
  units) expand to hundreds of thousands of polynomial-level instructions; the
  local `fhetch_sim` replay can take tens of seconds and gigabytes. To prove the
  path quickly, trim the model (fewer units / lower degree) or a small input
  batch — the record/replay mechanics are identical. The trace records the
  *circuit*, not the record count, so a small batch suffices.
- **Trace submission.** To target hardware instead of the local simulator,
  submit the `<app>_server_workload_*/` trace to the Niobium compilation service
  per the transport docs; nothing in the circuit changes.
- **Run artifacts are not source.** The key directory and the
  `*_server_workload_*/` trace directory are regenerated every run — `.gitignore`
  them; commit only `app-fog/` sources + `data/`.
- **This is not the DSL path.** The optional `nb` DSL (see
  `implementing-with-nb-dsl.md`) is a *different* front door that rewrites the
  computation in the DSL and generates OpenFHE. This add-on instead **reuses the
  finished OpenFHE app** through niobium-client's instrumented-OpenFHE entry
  point. Offer whichever the user wants; do not conflate them.

## Heavy and bootstrapped circuits — field notes

Applied across four apps (two MLPs, a bootstrapped LSTM, a deep CNN); these are
the things that actually bit:

- **Recording ≠ replay in cost.** Recording the trace is cheap and works even
  for bootstrapped circuits — `EvalBootstrap` records cleanly (a single LSTM
  step wrote a 2.6M-instruction trace including ~7,900 bootstrap-precompute
  polynomials). The *local `fhetch_sim` replay* is the expensive part: a single
  bootstrap at N=2¹⁶ (34-modulus chain, ~35 MB/poly) peaked ~14.5 GB and OOM-
  killed a ~16 GB box. **Do not assume a circuit that records will replay
  locally.**

- **Pick the verification to the circuit's weight:**
  - *Shallow (MLP-style):* record the whole circuit, replay locally, compare to
    the twin (0 flips, error in the encryption-noise band) — clean full green.
  - *Heavy/bootstrapped (deep RNN/CNN):* keep the emitted variant **full-shape**
    but default to **record-and-stop** (an `NB_FOG_NO_REPLAY` env that writes the
    trace and skips local replay). Hand the `*_workload_*/` trace to a
    large-memory host or the Niobium compilation service for the actual replay/
    deployment. Optionally offer a **bounded probe** (a `NB_FOG_STOP`/step-count
    env that records one gate / one step / pre-bootstrap) for a *smoke* that
    fits in memory and still proves rotations + Chebyshev + (recording of)
    bootstrap.

- **The recorder caches by (program_info + cache_parameters).** If you add a
  probe/trim mode, **put the mode in `cache_parameters`** or a stale trace from a
  previous mode will be replayed (symptom: `result('<name>') not found` because
  the cached trace has different probes). Also clear the `*_workload_*/`
  directory between runs to force a fresh recording.

- **Bootstrap plumbing for the Fog server:** call `EvalBootstrapSetup(...)`
  before recording, and ensure the bootstrap/rotation keys are provisioned to
  the server home and captured by `tag_keys`. Pass `--no-ring-dim-check` for
  N = 2¹⁶. These match niobium-client's own `bootstrap` example.

- **Run artifacts are not source.** The key dirs and `*_workload_*/` trace dirs
  regenerate each run — `.gitignore` them; commit only `app-fog/` sources + any
  small data subset.
