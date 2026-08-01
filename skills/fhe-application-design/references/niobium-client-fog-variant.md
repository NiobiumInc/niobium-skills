# The niobium-client (Fog) deployment of a finished OpenFHE app

This is the **Stage 10** run, done after the app is built and its CPU run is
validated and documented (the four programs keygen/encrypt/server/decrypt,
`run_test --cpu` green against the faithful twin, the two-process demo standing).
The *same* binary reaches the Fog. Recording the computation with
`niobium::compiler()` produces a FHETCH Polynomial IR trace (`.fhetch`); the
server has three run modes over it: `--cpu` runs plain OpenFHE (no trace);
`--sim` records the trace and reconstructs the result through the local
`fhetch_sim`; and the default records and dispatches the trace to the Niobium
Fog. `--sim` is the required local validation; the default needs a Fog API key.

## When to run it

After the app's CPU run is **validated** (`run_test --cpu` green). This is a
deployment mode of a known-good design, not a design path. Never design straight
to the Fog path, and never run the Fog mode before the CPU run passes.

## Where it lives

One directory in the **application repo** holds the whole app:

```
fhe-design/
├── app/          # the four-program app (keygen/encrypt/server/decrypt + run_test)
└── common.hpp    # the shared run_circuit() both run modes call
```

Keep the source in your repo; the FHE-dev container is only a build-and-run
dependency (see Build below). There is no separate Fog directory: the default run
mode is the Fog path.

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

if (!niobium::compiler().is_cache_valid()) {
    niobium::compiler().start();
    auto out = run_circuit(cc, model, inputs);     // the SHARED circuit
    niobium::compiler().probe("<output>", out);
    niobium::compiler().stop();                     // writes .fhetch + fhetch_replay.json
}
niobium::compiler().replay();                       // --sim: local fhetch_sim; default: the Fog
Ciphertext<DCRTPoly> ct_result;
niobium::compiler().result(cc, "<output>", ct_result);
// serialize ct_result for decrypt
```

`result()` also lets you diff the simulator output against OpenFHE's at the ring
level — a free "bit-identical" check (see Verification).

## Build and run (in the FHE-dev container)

The app builds against the SDK installed in the FHE-dev image via
`find_package(NiobiumFhetch)`, and runs through the `run-in-container.sh` wrapper
and `run_test.sh` that Stage 8 generates (the wrapper mounts the project at
`/work`, and `~/.fog` when present). Build once, then run the required local
validation (`--sim`):

```bash
./run-in-container.sh "cmake -S . -B build \
    -DCMAKE_PREFIX_PATH='/opt/niobium-client/vendor/lib/niobium-client;/opt/niobium-client/vendor/lib/openfhe' \
    && cmake --build build -j"
./run-in-container.sh "./run_test.sh --sim"      # record -> fhetch_sim -> compare vs twin
```

`run_test.sh` leaves the **minimum-ring-dimension check on** — it passes at
N = 2^16, so the flag is unnecessary. `--no-ring-dim-check` is an explicit opt-in
the user adds only to run at a sub-standard ring, never a default. `--cpu` runs the
plain OpenFHE path (the Stage 8 CPU gate). The image is one coherent build, so the
instrumented OpenFHE and `libnbfhetch` versions always match.

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

**Non-negotiable: the default mode must actually submit.** "Runs the server step
under `fog submit`" is a literal requirement, not a description of intent. When a
key is present, `run_test`'s no-flag path **must execute the server through
`fog submit`** — the concrete call is the app binary followed by a **required**
`--target`:

```bash
fog submit ./build/<app>_server <server_home> --target="${FOG_TARGET:?set a Fog target}"
```

`fog submit` provisions a Fog job, wires `NBCC_FHETCH_SERVER` to the assigned
worker, and runs your server against it; the server records the trace, dispatches
`replay()` to the worker, and reconstructs the result locally — so the rest of the
pipeline (copy `ct_result` back → decrypt → compare vs the twin) is identical to the
`--cpu` / `--sim` path. `--target` is required: **`FUNC_SIM`** is the Fog's
functional simulator (safe, no hardware); a hardware/compiler target is used for a
real deployment. (No `--no-ring-dim-check` here — the ring-dim guard is what would
catch an insecure or hardware-incompatible ring before it dispatches, and at
N = 2^16 it passes anyway; the Fog is the worst place to have bypassed it.)

**Do NOT emit a Fog branch that only preflights and exits.** A `run_test` whose
default mode — with a key present — prints "launch under `fog submit`" and returns
without ever calling `fog submit` is **incomplete**. This is the single most common
way this stage is faked, precisely because the Fog path is easy to leave
unexercised. Wire the real dispatch, and run it once against a live key (target
`FUNC_SIM` suffices) before treating the app as done.

## Verification gate

The Fog build must clear the **same** bar as the CPU app, plus a free extra:

- **Fog-replay vs the faithful twin**: max output error in the encryption-noise
  band and **0 decision flips** (identical criterion to `run_test`). The
  recorded design and parameters are unchanged, so this should hold.
- **Simulator vs OpenFHE (free)**: `result()` reconstructs the probe from the
  replayed trace; comparing it to OpenFHE's own computed ciphertext at the ring
  level should be **bit-identical**. A mismatch means the recorder/replayer did
  not reproduce the op stream — a trace problem, not a design problem.

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
  the server home and captured by `tag_keys`. (niobium-client's own `bootstrap`
  example passes `--no-ring-dim-check`, but only because it runs at a small test
  ring; at N = 2¹⁶ the guard passes, so leave it on.)

- **Run artifacts are not source.** The key dirs and `*_workload_*/` trace dirs
  regenerate each run — `.gitignore` them; commit only `app/` sources + any
  small data subset.
