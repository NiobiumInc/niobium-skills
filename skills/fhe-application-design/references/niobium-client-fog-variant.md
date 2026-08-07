# The niobium-client (Fog) deployment of a finished OpenFHE app

This is the **Stage 10** run, done after the app is built and its CPU run is
validated and documented (the four programs keygen/encrypt/server/decrypt,
`run_test --cpu` green against the faithful twin, the two-process demo standing).
The *same* binary reaches the Fog. Recording the computation with
`niobium::compiler()` produces a FHETCH Polynomial IR trace (`.fhetch`); the
server has four run modes over it: `--cpu` runs plain OpenFHE (no trace); `--sim`
records the trace **hollow** and reconstructs the result through the local
`fhetch_sim` (a rehearsal of the hollow Fog run); `--sim-full` records real math and
adds the ring-level ciphertext-identity check; and the default records **hollow** and
dispatches the trace to the Niobium Fog. `--sim` is the required local validation; the
default needs a Fog API key.

## When to run it

After the app's CPU run is **validated** (`run_test --cpu` green). This is a
deployment mode of a known-good design, not a design path. Never design straight
to the Fog path, and never run the Fog mode before the CPU run passes.

## Where it lives

The whole app is one directory in the **application repo**:

```
<app>/
├── app/          # the four programs (keygen/encrypt/server/decrypt) + common.hpp
├── data/         # model, inputs, and the twin/reference ledgers
└── run_test.sh   run-in-container.sh   Makefile   README.md
```

Keep the source in your repo; the FHE-dev container is only a build-and-run
dependency (see Build below). The default run mode is the Fog path, reached with a
bare `run_test.sh`.

## The governing rule: instrument, don't redesign

Both run modes must execute the **identical circuit** — same weights, parameters,
packing. The only difference is that the default (Fog) mode brackets the
computation with `niobium::compiler()` session calls. To make that guarantee
structural rather than a matter of discipline, **factor the circuit body into a
shared function** the server calls in both modes:

```cpp
// common.hpp
inline Ciphertext<DCRTPoly> run_circuit(
        CryptoContext<DCRTPoly>& cc, const Model& m,
        const std::vector<Ciphertext<DCRTPoly>>& x) {
    // ... the exact forward pass (Linear -> activation -> Linear -> ...) ...
    return result;
}
```

On `--cpu` the server calls `run_circuit(...)` directly and serializes the OpenFHE
result; in the default mode it calls the same function between `start()` and
`stop()`. The math cannot drift because there is only one copy of it.

## Only the server is instrumented

The compute is the thing the Fog accelerates, so only the **server** is
instrumented. `keygen` / `encrypt` / `decrypt` are the client-side programs and
are reused essentially unchanged (packaged as the example's `client` and
`decrypt`, matching niobium-client's example triple convention).

## The recording pattern

When recording (`--sim` or the default Fog mode), the server follows
niobium-client's example convention:

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

// load context, input ciphertexts, eval keys, model (as in the --cpu path)
niobium::compiler().capture_crypto_context(cc);
for (each input ct) niobium::compiler().tag_input("<name>", ct);
niobium::compiler().tag_keys(cc);

bool hollow_record = niobium::compiler().is_hollow_mode();  // --hollow, already consumed by init()

if (!niobium::compiler().is_cache_valid()) {
    niobium::compiler().enable_hollow_mode(hollow_record);  // hollow: skip real math on the record pass
    niobium::compiler().start();
    auto out = run_circuit(cc, model, inputs);     // the SHARED circuit
    niobium::compiler().probe("<output>", out);
    niobium::compiler().stop();                     // writes .fhetch + fhetch_replay.json
    niobium::compiler().enable_hollow_mode(false);  // restore for the ring-level baseline below
}
niobium::compiler().replay();                       // --sim: local fhetch_sim; default: the Fog
Ciphertext<DCRTPoly> ct_result;
niobium::compiler().result(cc, "<output>", ct_result);  // the REPLAY output — valid even under hollow
// serialize ct_result for decrypt
```

`result()` returns the **replay** output, which is valid in every mode — including
hollow, where the record pass produced no real ciphertext. On a real-math record you
can additionally diff it against OpenFHE's own record-pass ciphertext at the ring level
— a free "bit-identical" check (see Verification). Under hollow the record-pass
ciphertext is garbage by design, so that particular cross-check is a real-record-only
affordance.

## Hollow recording and the run modes

Recording the trace does not need the real polynomial math: the replay (local
`fhetch_sim` or the Fog) reconstructs the true values from the recorded op stream.
Hollow recording (`enable_hollow_mode(true)`, driven by the `--hollow` flag) skips that
math on the record pass, cutting record time substantially on large circuits while
producing the same trace. `init()` consumes `--hollow` and compacts it out of argv, so
recover it with `niobium::compiler().is_hollow_mode()` after `init()` (never re-parse
argv for it), then bracket the circuit with `enable_hollow_mode(hollow_record)` …
`enable_hollow_mode(false)` as shown above.

The generated harness makes recording mode-driven:

- **Default (Fog): hollow.** The record pass that precedes `fog submit` runs hollow, and
  the Fog reconstructs the result on replay.
- **`--sim`: hollow.** A faithful local rehearsal of the Fog run — hollow record → local
  `fhetch_sim` replay → compare the decrypted result to the twin.
- **`--sim-full`: real math.** The thorough ground-truth run — real record → replay,
  plus the ring-level ciphertext-identity check (which needs a real record-pass
  baseline).
- **`--cpu`: not applicable** (no recorder).

**Keep `--sim` and `--sim-full` both, and document running them together.** `--sim`
mirrors what deploys (hollow); `--sim-full` is the real-math ground truth with the
byte-level ciphertext check. Their decrypted results must agree — a divergence points at
a hollow-recording fidelity bug in the toolchain, not the app. That cross-check is the
whole reason both modes exist; do not collapse them.

**Correctness precondition (met by the pattern above):** every plaintext operand inside
the record bracket must be captured. The library captures its own internal
`EvalLogistic`/`EvalChebyshev*` coefficients and inline plaintexts automatically; the
operands *you* build must be `tag_input`'d before `start()` (the same rule the `--sim`
caveats below state).

## Build and run (in the FHE-dev container)

The app builds against the SDK in the FHE-dev image via `find_package(NiobiumFhetch)`
and runs through the generated `run-in-container.sh` wrapper and `run_test.sh`. See
[run-harness.md](run-harness.md) for the scripts, the build command, the three run
modes, and the `RINGCHK` / `FOG_TARGET` / `NREC` knobs. The image is one coherent
build, so the instrumented OpenFHE and `libnbfhetch` versions always match. Before
deploying to the Fog, run the required local validation,
`./run-in-container.sh "./run_test.sh --sim"`, which generates the trace, runs it
through `fhetch_sim`, and compares the result against the twin.

**Deploying to the Fog (the default).** The default (Fog) mode dispatches the trace to
the Niobium Fog and needs an API key; the server preflights for one
(`~/.fog/credentials` via `fog login`, or `FOG_API_TOKEN`) and prints a friendly
sign-in / sign-up pointer if it is missing, with `--sim` as the account-free
alternative. Mint a key once, then deploy through the wrapper (which mounts
`~/.fog`); the bare `run_test.sh` (no flag) runs the server step under `fog submit`:

```bash
docker run --rm -it -v "$HOME/.fog":/root/.fog ghcr.io/niobiuminc/fhe-dev:v0.13.0 fog login
./run-in-container.sh "./run_test.sh"
```

**Non-negotiable: the default mode must actually submit.** When a key is present,
`run_test`'s no-flag path **must execute the server through `fog submit`**, with a
**required** `--target`:

```bash
FOG_TARGET="${FOG_TARGET:-FOG}"   # the real Niobium Fog; FUNC_SIM = hardware-free simulator
fog submit ./build/<app>_server <server_home> --target="$FOG_TARGET"
```

`fog submit` provisions a Fog job, wires `NBCC_FHETCH_SERVER` to the assigned
worker, and runs your server against it; the server generates the trace, dispatches
`replay()` to the worker, and reconstructs the result locally, so the downstream
pipeline (copy `ct_result` back, decrypt, compare vs the twin) is identical to the
`--cpu` / `--sim` path. `--target` defaults to the real Fog (`FOG`), never a
simulator; `FUNC_SIM` (`FOG_TARGET=FUNC_SIM`) is an explicit hardware-free
alternative. The Fog runs exactly N = 2^16: keep the ring-dim guard on for every
Fog dispatch (it is never bypassed there), so a non-2^16 ring cannot reach the Fog.
The `--no-ring-dim-check` bypass is only for local `--cpu` / `--sim` testing of a
smaller ring. A default mode
that, with a key present, prints "launch under `fog submit`" and exits without
calling it is **incomplete**; this is the single most common way this stage is
faked. Wire the real dispatch and run it once against a live key (target
`FUNC_SIM` suffices) before treating the app as done.

## Verification gate

The Fog build must clear the **same** bar as the CPU app, plus a free extra:

- **Fog-replay vs the faithful twin**: max output error in the encryption-noise
  band and **0 decision flips** (identical criterion to `run_test`). The
  recorded design and parameters are unchanged, so this should hold.
- **Simulator vs OpenFHE (free, `--sim-full`)**: on a real-math record `result()`
  reconstructs the probe from the replayed trace; comparing it to OpenFHE's own
  computed ciphertext at the ring level should be **bit-identical**. A mismatch means
  the recorder/replayer did not reproduce the op stream — a trace problem, not a design
  problem. This check needs a real record, so it runs under `--sim-full`; the hollow
  `--sim`/Fog default has no real record-pass ciphertext, and its gate is
  replay-vs-twin above.
- **`--sim` vs `--sim-full` agree**: the decrypted results of the hollow and real
  records must match. A divergence is a hollow-recording fidelity problem in the
  toolchain, not a design fault — surfacing exactly that is why both modes exist.

## Caveats and notes

- **A `--sim` run that passed `--cpu` but diverges is almost always one of two
  authoring slips, not a recorder or design fault.** The FHETCH recorder reproduces
  in-circuit `EvalLogistic` / `EvalChebyshev*` and rotations bit-identically; when a
  recorded run misbehaves, check these first:
  - **Tag every plaintext you build before `start()`.** Encode and `tag_input` any
    plaintext operands you construct (slot masks, hand-built coefficient plaintexts)
    *before* the recording `start()`, or replay reads uninitialized values — symptom
    `[FHETCH_SIM] ... read from uninitialized address` and a wrong result. The library's
    own internal `EvalLogistic` / `EvalChebyshev*` coefficient plaintexts are captured
    automatically; only the operands *you* create need tagging.
  - **Keep every slot fed to an in-circuit activation within its `[lo,hi]` interval.**
    Zero or isolate the non-target slots (e.g. multiply by a slot-0 mask) before
    `EvalLogistic` / `EvalChebyshev*`, or `Decode` fails with "approximation error too
    high" — on plain OpenFHE as well as on replay, since the Chebyshev fit is only valid
    in range.

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
  them; commit only `app/` sources + `data/`.
- **This is not the DSL path.** The `nb` DSL (see
  `implementing-with-nb-dsl.md`) is a *different* front door that rewrites the
  computation in the DSL and generates OpenFHE. This stage instead **reuses the
  finished OpenFHE app** through niobium-client's instrumented-OpenFHE entry
  point. Do not conflate them.

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
  the server home and captured by `tag_keys`. (niobium-client's own `bootstrap`
  example passes `--no-ring-dim-check`, but only because it runs locally at a small
  test ring; a Fog run uses N = 2¹⁶ with the guard on.)

- **Run artifacts are not source.** The key dirs and `*_workload_*/` trace dirs
  regenerate each run — `.gitignore` them; commit only `app/` sources + any
  small data subset.
