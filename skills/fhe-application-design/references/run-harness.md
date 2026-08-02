# The generated run harness (container wrapper, run_test, Makefile)

Three small files sit at the top of the application directory and keep every
build-and-run command short. They are generated in Stage 8 and used through
Stage 10. Build and run happen inside the FHE-dev container; only `clean` runs on
the host.

Contents:
- [run-in-container.sh](#run-in-containersh) — run any command inside the image
- [run_test.sh](#run_testsh) — the keygen → encrypt → server → decrypt pipeline, three modes
- [Makefile](#makefile) — the clean target
- [Build and validate](#build-and-validate) — the first build and CPU run

## run-in-container.sh

Runs a command in the FHE-dev image with the project mounted at `/work` (and
`~/.fog` when present, so the Fog mode sees the API key):

```bash
#!/usr/bin/env bash
set -euo pipefail
IMAGE="${FHE_DEV_IMAGE:-ghcr.io/niobiuminc/fhe-dev:v0.13.0}"
FOG=(); [ -d "$HOME/.fog" ] && FOG=(-v "$HOME/.fog:/root/.fog")
exec docker run --rm -v "$PWD":/work -w /work "${FOG[@]}" "$IMAGE" bash -c "$*"
```

Give it a `--help` (and bare no-arg) path that prints the common invocations:
`./run_test.sh`, `./run_test.sh --cpu`, `./run_test.sh --sim`, `./run_test.sh
--help`, plus the build command, so a user finds the modes without opening the
file.

## run_test.sh

Orchestrates keygen → encrypt → server → decrypt across a client home and a server
home (the server refuses to start if a secret key is in its home; include that
negative test), forwards the mode to `server`, then reports results in two tiers.

**Lead the printed summary with the application's own quality metrics**: the
model's task performance on the decrypted outputs, the numbers a user weighing FHE
for this workload would look at first (accuracy, area under the curve, error
distribution, or decision counts, as fits the task). Present the FHE-specific
comparison (decrypted output against the faithful twin, the encryption error) and
the deployment profile (timings, boundary sizes, peak server memory) below that, as
second-tier evidence.

Three run modes:

- **(no flag) the Fog, the default.** Targets the Niobium Fog (Stage 10). It
  preflights for an API key (printing the sign-in / sign-up pointer if none is
  found), and with a key present dispatches the server under `fog submit
  … --target=`. A print-and-exit stub that never submits is incomplete; see
  [niobium-client-fog-variant.md](niobium-client-fog-variant.md) for the concrete
  call. The Fog target defaults to the real Fog (`FOG`); a simulator is never the
  default. `FOG_TARGET=FUNC_SIM` is an explicit hardware-free opt-in.
- **`--cpu`** runs plain OpenFHE on the local CPU (the Stage 8 correctness gate).
- **`--sim`** generates the FHETCH trace and runs it through the local simulator
  (`fhetch_sim`), plus the free bit-identical ring-level check against OpenFHE.

Provide `-h`/`--help` listing the three modes and the env knobs:

- `FOG_TARGET` sets the Fog target for the default mode (default `FOG`;
  `FOG_TARGET=FUNC_SIM` selects the hardware-free functional simulator).
- `RINGCHK` set to `--no-ring-dim-check` bypasses the minimum-ring-dimension
  security floor. The check passes at N = 2^16, so this is an opt-in for a
  deliberately small ring, not a default.
- `NREC` sets how many records to score (a small default for `--sim` and the Fog,
  a larger one for `--cpu`).

## Makefile

A `clean` target that removes everything a build or a run regenerates: the
`build/` tree, the per-run homes (the `run_cpu/` / `run_sim/` / `run_fog/` dirs and
any root `client_home/` / `server_home/`), and the `*_server_workload_*/` FHETCH
trace directories. **List the run-home directories explicitly; never `rm -rf run_*`**,
because that glob also matches `run_test.sh` and deletes the orchestrator (the same
trap catches any generated script whose name a clean glob can hit). Keep the paths
in sync with what the scripts create and with `.gitignore`.

## Build and validate

Build once, then validate locally on CPU, both through the wrapper:

```bash
./run-in-container.sh "cmake -S . -B build \
    -DCMAKE_PREFIX_PATH='/opt/niobium-client/vendor/lib/niobium-client;/opt/niobium-client/vendor/lib/openfhe' \
    && cmake --build build -j"
./run-in-container.sh "./run_test.sh --cpu"
```
