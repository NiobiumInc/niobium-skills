# The generated run harness (container wrapper, run_test, Makefile, .gitignore)

Three small files sit at the top of the application directory and keep every
build-and-run command short, and a generated `.gitignore` keeps the working tree
clean. They are generated in Stage 8 and used through Stage 10. Build and run happen
inside the FHE-dev container; only `clean` runs on the host.

Contents:
- [run-in-container.sh](#run-in-containersh) — run any command inside the image
- [run_test.sh](#run_testsh) — the keygen → encrypt → server → decrypt pipeline, four modes
- [Makefile](#makefile) — the clean target
- [.gitignore](#gitignore) — ignore the build tree and per-run artifacts
- [Build and validate](#build-and-validate) — the first build and CPU run

## run-in-container.sh

Runs a command against the FHE build-and-run environment. There are two
provisioning modes behind one interface, so the same call site works either way:

- **Container mode (default).** The command runs inside the FHE-dev image with
  the project mounted at `/work` (and `~/.fog` when present, so the Fog path sees
  the API key). Nothing but Docker is needed on the host.
- **Local mode.** Set `NIOBIUM_CLIENT_DIR` to a built niobium-client checkout on
  the host and the command runs directly on the host instead of in a container.
  The toolchain, the `fog` CLI, and the `nbc` compiler come from that build, so no
  Docker is involved. See `references/environment-setup.md` Path B for how to
  produce that checkout.

```bash
#!/usr/bin/env bash
# One interface, two provisioning modes. NIOBIUM_CLIENT_DIR set -> run on the
# host against a local niobium-client build; unset -> run inside the FHE-dev image.
set -euo pipefail
if [ -n "${NIOBIUM_CLIENT_DIR:-}" ]; then
    exec bash -c "$*"                                          # local: run in place on the host (inherits the shell env)
fi
IMAGE="${FHE_DEV_IMAGE:-ghcr.io/niobiuminc/fhe-dev:latest}"   # tracks the current image; for a reproducible app pin a version: FHE_DEV_IMAGE=ghcr.io/niobiuminc/fhe-dev:vX.Y.Z
FOG=(); [ -d "$HOME/.fog" ] && FOG=(-v "$HOME/.fog:/root/.fog")
# Forward the run knobs from the host env so `RING_DIM=… ./run-in-container.sh "…"`
# behaves the same in the container as it does in local mode (docker does not
# inherit the caller's environment). Only knobs actually set are passed.
ENVFWD=(); for v in RING_DIM RINGCHK RECORD NREC N_ENC FOG_TARGET; do [ -n "${!v:-}" ] && ENVFWD+=(-e "$v"); done
# ${arr[@]+"${arr[@]}"} guards empty-array expansion under `set -u` on bash 3.2
# (macOS default), which otherwise aborts with "unbound variable".
exec docker run --rm -v "$PWD":/work -w /work ${FOG[@]+"${FOG[@]}"} ${ENVFWD[@]+"${ENVFWD[@]}"} "$IMAGE" bash -c "$*"
```

In local mode the app runs in place, so the `fog` CLI and `nbc` must already be on
`PATH` (Path B installs `fog` to `~/.local/bin` and invokes `nbc` from the
checkout). In container mode they ship in the image. Host env knobs
(`RING_DIM`/`RINGCHK`/`RECORD`/`NREC`/`N_ENC`/`FOG_TARGET`) reach the program in both modes:
local mode inherits the shell env, and container mode forwards them with `-e` (an
app-specific knob not in that list must be set inside the quoted command,
`./run-in-container.sh "FOO=bar ./run_test.sh"`). Neither mode changes the commands
below.

Give it a `--help` (and bare no-arg) path that prints the common invocations:
`./run_test.sh`, `./run_test.sh --cpu`, `./run_test.sh --sim`, `./run_test.sh
--help`, plus the build command, and names which provisioning mode is active
(container by default, local when `NIOBIUM_CLIENT_DIR` is set), so a user finds
the modes without opening the file.

## run_test.sh

Orchestrates keygen → encrypt → server → decrypt across a client home and a server
home (the server refuses to start if a secret key is in its home; include that
negative test), forwards the mode to `server`, then reports results in two tiers.

**Lead the printed summary with the application's own quality metrics**: task
performance measured against the ground-truth labels in the test set (accuracy,
area under the curve, precision/recall, or task-appropriate error). This needs a
labeled test set with real ground truth (Stage 3); the model's output distribution
(decision counts, mean score) is no substitute, since without an expected baseline
those numbers are not interpretable. Present the FHE comparison (decrypted output
against the faithful twin) and the deployment profile (timings, boundary sizes,
peak server memory) below that, as second-tier evidence.

When the user is new to FHE, label these numbers in plain terms in the summary the
user reads: what the quality metric means for the task, and what the fidelity
comparison is telling them (that encryption did not change the answers), rather than
a bare parameter or error dump. The raw numbers still belong in the report tables;
`references/explaining-fhe-to-newcomers.md` covers how to phrase them.

Four run modes:

- **(no flag) the Fog, the default.** Targets the Niobium Fog (Stage 10). It
  preflights for an API key (printing the sign-in / sign-up pointer if none is
  found), and with a key present dispatches the server under `fog submit
  … --target=`. A print-and-exit stub that never submits is incomplete; see
  [niobium-client-fog-variant.md](niobium-client-fog-variant.md) for the concrete
  call. The Fog target defaults to the real Fog (`FOG`); a simulator is never the
  default. `FOG_TARGET=FUNC_SIM` is an explicit hardware-free opt-in. The record
  pass that precedes dispatch runs **hollow** (fast trace generation; the replay
  reconstructs the real values) — see "Hollow recording and the run modes" in
  [niobium-client-fog-variant.md](niobium-client-fog-variant.md).
- **`--cpu`** runs plain OpenFHE on the local CPU (the Stage 8 correctness gate).
- **`--sim`** records **hollow** and runs the trace through the local simulator
  (`fhetch_sim`), comparing the decrypted result to the twin. Recording hollow, it is
  a faithful local rehearsal of the Fog run.
- **`--sim-full`** records **real math** and runs the same local replay, plus the free
  bit-identical ring-level ciphertext-identity check against OpenFHE (which needs a
  real record-pass baseline). It is the thorough ground-truth run.

**Why two simulator modes.** `--sim` mirrors what deploys (hollow); `--sim-full` is the
real-math ground truth with the byte-level ciphertext check. Running both and comparing
their decrypted results is the standing cross-check for **hollow-recording fidelity**:
they must agree, and a divergence points at a hollow-mode bug in the toolchain rather
than the app. Keep both; do not collapse them.

Provide `-h`/`--help` listing the four modes and the env knobs:

- `FOG_TARGET` sets the Fog target for the default mode (default `FOG`;
  `FOG_TARGET=FUNC_SIM` selects the hardware-free functional simulator).
- `RINGCHK` set to `--no-ring-dim-check` bypasses the minimum-ring-dimension
  security floor for **local** `--cpu` / `--sim` runs only, so you can test a
  deliberately small ring (e.g. 2^15, or a toy 2^10) fast. It is never forwarded
  to a Fog run: the Fog runs exactly N = 2^16 and its ring-dim guard is always on,
  so a non-2^16 ring cannot reach the Fog.
- `NREC` sets how many records to score. **Default it to the deployment's unit of
  work, not to the sample size the fidelity gate wants.** For `per_record` packing
  that is ONE record, selected by a `RECORD`-style index so the run reads as
  "score mine"; for `batched` it is one batch. `NREC` above that default is a
  **validation sweep**, opt-in and labelled as such in the output, which is the
  only thing the multi-record loop is for: giving the FHE-vs-twin gate more
  samples.
- **A sweep must not share keys across independent encryptors.** If Stage 1 says
  each record's owner holds its own key, generate a key set per record inside the
  sweep and say so in the output. Running one `keygen` and scoring N records under
  it contradicts the privacy model the application claims, and a reader will
  conclude the server batches everyone together under one key.
- **Keep the per-request result separate from offline model quality.** The run
  should lead with what the deployed system returns for the record it scored, then
  report population metrics separately, labelled as computed in the clear over a
  labeled set. Printing only aggregates makes an encrypted single-record protocol
  read as a group analysis, and makes plaintext validation numbers look like
  output of the encrypted run.

Generate `run_test.sh` from the skeleton below. Keep the unbracketed lines as
they are, in particular the mode parsing, the home provisioning, the negative
test, the key preflight, and the `fog submit` dispatch; fill the `<...>`
placeholders and the three report blocks for the application. Step 5 below is
written for **per-record** packing (the `for` loop over `NREC`). For a **batched**
design (column-major, records across slots, which the single-encryptor case in
SKILL.md recommends) swap step 5 for the batched form in "Batched step 5" below —
one `encrypt`, one server run, one decrypt over the batch — and keep everything
else (home provisioning, negative test, key preflight, dispatch, reporting) as is.

```bash
#!/usr/bin/env bash
# Generated by the Niobium FHE Application Design AI Assistant (FHEanna).
set -euo pipefail

usage() {
  cat <<'EOF'
run_test.sh — <app>: keygen -> encrypt -> server -> decrypt, then compare to the faithful twin.
Usage: ./run_test.sh [--cpu | --sim | --sim-full | -h]
  (no flag)   dispatch to the Niobium Fog (default; records hollow, needs an API key)
  --cpu       plain-OpenFHE local validation
  --sim       hollow record -> local fhetch_sim replay -> twin compare (a Fog rehearsal)
  --sim-full  real-math record -> replay + ring-level ciphertext-identity check vs OpenFHE
              (compare --sim and --sim-full to surface any hollow-recording divergence)
Env: FOG_TARGET (default FOG; FUNC_SIM = hardware-free simulator)
     RINGCHK   (set to --no-ring-dim-check only for a deliberately small ring)
     RECORD    which record to score (default 0)
     NREC      validation-sweep size (default 1)
EOF
}

MODE="fog"; FLAG=""; SIMFULL=0
case "${1:-}" in
  --cpu) MODE="cpu"; FLAG="--cpu";;
  --sim) MODE="sim"; FLAG="--sim";;
  --sim-full) MODE="sim"; FLAG="--sim"; SIMFULL=1;;
  "")    MODE="fog";;
  -h|--help) usage; exit 0;;
  *) usage; exit 2;;
esac

FOG_TARGET="${FOG_TARGET:-FOG}"   # the real Niobium Fog; FUNC_SIM is the explicit hardware-free opt-in
RINGCHK="${RINGCHK:-}"
# Hollow record on the Fog default and --sim (fast, mirrors the Fog run); real math on
# --sim-full (so the server's ring-level ciphertext-identity check has a real baseline)
# and --cpu. init() consumes --hollow and compacts argv; the server recovers it via
# is_hollow_mode() and brackets the circuit with enable_hollow_mode().
HOLLOW_FLAG=""
case "$MODE" in
  fog) HOLLOW_FLAG="--hollow";;
  sim) [ "$SIMFULL" = 1 ] || HOLLOW_FLAG="--hollow";;
esac
# The deployment's unit of work is the default: one record for per_record packing,
# one batch for batched. NREC above it is an opt-in validation sweep.
RECORD="${RECORD:-0}"             # which record to score
NREC="${NREC:-1}"

ROOT="$(cd "$(dirname "$0")" && pwd)"
# --sim and --sim-full get separate run dirs so the sim-vs-sim-full cross-check
# compares two independent runs instead of clobbering one (both set MODE=sim).
BUILD="$ROOT/build"; RUN="$ROOT/run_${MODE}"; [ "$SIMFULL" = 1 ] && RUN="$ROOT/run_sim-full"
CLIENT="$RUN/client_home"; SERVER="$RUN/server_home"
# Clear the per-run home AND the FHETCH trace cache each run, so --sim-full records
# real math (for its ring-level identity check) instead of reusing a hollow --sim trace.
rm -rf "$RUN" "$ROOT"/<app>_server_workload_* "$ROOT"/nbcc_fhetch_replay_source_*; mkdir -p "$CLIENT" "$SERVER"

# 1. keygen writes ALL keys into the client home
"$BUILD/<app>_keygen" "$CLIENT"

# 2. provision the server home: context, public + eval keys, model. NO secret key, NO inputs.
cp "$CLIENT/cc.bin" "$CLIENT/pk.bin" "$CLIENT/mk.bin" "$CLIENT/rk.bin" "$SERVER/"
cp "$ROOT/<model file>" "$SERVER/"
[ ! -f "$SERVER/sk.bin" ] || { echo "[FATAL] secret key in server home"; exit 1; }

# 3. negative test: the server must refuse to start with a secret key in its home
cp "$CLIENT/sk.bin" "$SERVER/sk.bin"
if "$BUILD/<app>_server" "$SERVER" $FLAG $RINGCHK >/dev/null 2>&1; then
  echo "[FATAL] server started with a secret key present"; exit 1
fi
rm -f "$SERVER/sk.bin"; echo "negative test: server refused a planted secret key"

# 4. Fog mode: preflight for an API key; with a key present, the dispatch below is mandatory
if [ "$MODE" = "fog" ] && [ ! -f "$HOME/.fog/credentials" ] && [ -z "${FOG_API_TOKEN:-}" ]; then
  echo "No Fog API key found — not dispatching."
  echo "  Sign in:  fog login        Sign up:  https://console.niobium.co/request-account"
  echo "  Account-free local validation:  ./run_test.sh --sim"
  exit 0
fi

# 5. encrypt -> server -> decrypt (bounds enforcement lives in <app>_encrypt)
for ((i=0; i<NREC; i++)); do
  "$BUILD/<app>_encrypt" "$CLIENT" <input args> "$CLIENT/ct_x_$i.bin"
  cp "$CLIENT/ct_x_$i.bin" "$SERVER/ct_x.bin"          # only ciphertext crosses
  if [ "$MODE" = "fog" ]; then
    # THE DEFAULT PATH: the server runs under `fog submit`. A default mode that
    # preflights and exits without ever calling `fog submit` is incomplete.
    # The Fog runs exactly N = 2^16. The ring-dim guard stays on for every Fog
    # dispatch (RINGCHK, the local-testing bypass, is not forwarded here), so a
    # non-2^16 ring cannot reach the Fog.
    fog submit "$BUILD/<app>_server" "$SERVER" $HOLLOW_FLAG --target="$FOG_TARGET"
  else
    # --sim passes --hollow (server records hollow, skips its ring-level check);
    # --sim-full omits it (real record, so the server's ring-level check runs).
    "$BUILD/<app>_server" "$SERVER" $FLAG $HOLLOW_FLAG $RINGCHK   # wrap to capture wall-clock + peak RSS
    # For peak RSS, wrap the server in a small Python parent that reads
    # resource.getrusage(RUSAGE_CHILDREN).ru_maxrss (portable, needs no packages,
    # and works in the image — /usr/bin/time is not installed there). ru_maxrss is
    # bytes on macOS, kilobytes on Linux; label the unit accordingly.
  fi
  cp "$SERVER/ct_result.bin" "$CLIENT/ct_result_$i.bin"
  "$BUILD/<app>_decrypt" "$CLIENT" "$CLIENT/ct_result_$i.bin" >> "$RUN/decrypted.csv"
done

# 6. report, in this order:
#   (a) LEAD: the application's own quality metrics vs the ground-truth labels
#       (task-appropriate: accuracy/AUC/precision-recall or regression error, plus the base rate)
<app-specific quality block>
#   (b) encryption fidelity: decrypted output vs the faithful twin; max/mean error against the
#       Stage 7 noise tolerance and decision flips. This comparison is the PASS/FAIL exit code.
<twin comparison block; exit nonzero on FAIL>
#   (c) deployment profile: per-stage wall-clock, peak server RSS (local modes), boundary sizes
<profile block>
```

### Batched step 5 (single-encryptor, column-major — the recommended default)

Step 5 above is the **per-record** form. For a single-encryptor design that packs
records across slots (SKILL.md's recommended packing for that case), replace the
`for` loop with **one** encrypt, **one** server run, and **one** decrypt over the
whole batch. `NREC` is then the number of records packed into the batch (≤ the slot
count), not a loop count. Steps 1–4 and 6 are unchanged, and the server branch
(Fog / `--sim` / `--sim-full` / `--cpu`, with `$HOLLOW_FLAG` / `$RINGCHK` and the
peak-RSS wrap) is identical to the loop version — only the encrypt and decrypt
around it change:

```bash
# 5 (batched). encrypt once -> one ciphertext per feature column (bounds enforced in <app>_encrypt)
"$BUILD/<app>_encrypt" "$CLIENT" <input args>          # writes $CLIENT/ct_x_f0.bin ... ct_x_fK.bin
cp "$CLIENT"/ct_x_f*.bin "$SERVER/"                     # only ciphertext crosses
if [ "$MODE" = "fog" ]; then
  fog submit "$BUILD/<app>_server" "$SERVER" $HOLLOW_FLAG --target="$FOG_TARGET"
else
  "$BUILD/<app>_server" "$SERVER" $FLAG $HOLLOW_FLAG $RINGCHK   # wrap for wall-clock + peak RSS (rusage, as above)
fi
cp "$SERVER"/ct_result*.bin "$CLIENT/"                 # one result ciphertext (or a few, e.g. per class)
"$BUILD/<app>_decrypt" "$CLIENT" > "$RUN/decrypted.csv" # decrypt unpacks all NREC records' outputs at once
```

Choose per-record only when the design genuinely encrypts one record at a time
(e.g. independent encryptors, or a per-record request/response shape); the batched
form is the default for the single-encryptor full-book case.

### On the DSL path

The skeleton above uses the OpenFHE-path binary interface
(`"$BUILD/<app>_keygen" "$CLIENT"`). Generated DSL binaries differ; keep the
skeleton's structure (the four modes, the two homes, the negative test, the
reporting order) and change:

- **Binary interface: a profile index plus the working directory, not a home
  argument.** Each generated `@stage("name")` binary takes the profile index as
  `argv[1]` and reads/writes relative to the current directory (`root()` is the
  process CWD). Provision each home and `cd` into it before running the stage:
  `(cd "$SERVER" && "$BUILD/<stage-name>" "$PROFILE")`, with `BUILD=nb_out/build`.
  The binaries are named for their `@stage`s (`key_generation`, `encrypt_...`, the
  server-compute stage, `decrypt_...`), not
  `<app>_keygen`/`_encrypt`/`_server`/`_decrypt`.
- **Sim modes need two server invocations; `--cpu` also records a trace.** The
  generated `@hardware` server replays only on a cache-valid run: a first `--sim` /
  `--sim-full` invocation records the trace and stops, serializing a placeholder, and
  only a second invocation calls `replay()` and reconstructs the real values. Clear
  the trace cache once at the top, then invoke the server **twice** for the sim modes
  (a single invocation decrypts garbage). `--cpu` computes real math in one pass but
  still records, so a `<stage>_workload_*` dir and a `.fhetch` file appear under
  `--cpu` too — cover them in `.gitignore` and the Makefile `clean`.
- **Negative test: assert key absence, don't expect the server to refuse.** The
  generated `@server` binary has no runtime secret-key guard (the compile-time
  `@server` / `SecretKey` split is the guarantee), so `run_test` asserts there is no
  `sk.bin` in the server home before launching, rather than planting one and
  expecting a nonzero exit.
- **Server key set and local-replay routing.** Provision `cc`/`pk`/`mk`/`rk` into
  the server home: the generated `@server` hard-requires `rk.bin`
  (EvalSum/automorphism keys) and aborts with "Failed to load EvalAutomorphism key"
  without it, even for a rotation-free circuit (the non-minimal-keygen pitfall in
  `implementing-with-nb-dsl.md`). Local `--sim`/`--sim-full` replay is routed by
  `NBCC_FHETCH_DRIVER`
  (`$NIOBIUM_CLIENT_DIR/vendor/niobium-fhetch/build/tests/fhetch_driver/fhetch_driver`)
  plus `LD_LIBRARY_PATH` (and `DYLD_LIBRARY_PATH` on macOS).
- **Small-ring local testing.** The `@hardware` record path enforces the N = 2^16
  hardware floor and aborts a deliberately small local ring ("Ring dimension … not
  compatible with Niobium Hardware") unless `--no-ring-dim-check` is passed to the
  stage binary — forward `$RINGCHK` to the DSL server for local `--cpu`/`--sim` as
  the OpenFHE skeleton does. Set the small ring itself via a `ring_dim` field on the
  `Instance` struct (a literal `ring_dim` in the `scheme` block fixes it for all
  profiles; `scheme.override(ring_dim:)` is a no-op).

## Makefile

A `clean` target that removes everything a build or a run regenerates: the
`build/` tree, the per-run homes (`run_*/`, including the two-process demo's, plus
any root `client_home/` / `server_home/`), and the `*_server_workload_*/` FHETCH
trace directories. **Match run homes with `run_*/`, never bare `run_*`**: the
trailing slash matches directories only, so it cannot delete `run_test.sh`. Keep
these paths in sync with the `.gitignore` below.

## .gitignore

Ship a `.gitignore` with the application, so a fresh clone carries the sources and
nothing a build or a run regenerates. It covers everything `clean` removes plus local
tooling that must never be committed. The fixed lines are the same for every
application; add this application's own trace and profile directory names by hand, and
match the build tree to the implementation path. This is a required deliverable, not an
afterthought: without it the run artifacts (per-run homes full of keys and ciphertexts,
trace directories) show up as untracked and get committed by accident.

Raw run outputs are build artifacts. Each mode writes them under its per-run
`run_<mode>/` directory (the client and server homes, decrypted outputs, and the
timing, profile, and comparison captures the summary is computed from) and into the
trace and profile directories, all ignored by directory type alongside the build
tree, so a run regenerates them and commits nothing. The committed reports (the
results report and the run README) are the authored deliverables that carry the
numbers forward; a run does not write to them.

```gitignore
# Generated by the Niobium FHE Application Design AI Assistant (FHEanna).
# Build tree (OpenFHE path)
/build/
# Build tree (DSL path: keep the generated nb_out sources, ignore its build/)
/nb_out/build/
# Per-run homes provisioned by run_test.sh and the two-process demo (keys, ciphertexts).
# Directory-only glob: covers every run mode, and cannot match run_test.sh.
/run_*/
client_home/
server_home/
# Local tooling that must never be committed
.claude/
.agents/
.venv/
__pycache__/
# Toolchain replay artifacts
/nbcc_fhetch_replay_source_*/
/fhetch_driver_source_*/
# App-specific: name this application's trace and profile directories
/<app>_server_workload_*/
/<app>_profile_*/
```

## Build and validate

Build once, then validate locally on CPU, both through the wrapper. `NC` is the
built niobium-client: `/opt/niobium-client` inside the image, or the local checkout
in `NIOBIUM_CLIENT_DIR` (the default covers the image, and the layout under it is
identical either way). The build command depends on the implementation path.

**OpenFHE path** — the app's own `CMakeLists.txt` finds the SDK with
`find_package(NiobiumFhetch)` off `CMAKE_PREFIX_PATH`:

```bash
NC="${NIOBIUM_CLIENT_DIR:-/opt/niobium-client}"
./run-in-container.sh "cmake -S . -B build \
    -DCMAKE_PREFIX_PATH='$NC/vendor/lib/niobium-client;$NC/vendor/lib/openfhe' \
    && cmake --build build -j"
./run-in-container.sh "./run_test.sh --cpu"
```

**DSL path** — build the generated `nb_out/` project, which locates the SDK via
`NIOBIUM_CLIENT_ROOT` (it does not use `find_package` / `CMAKE_PREFIX_PATH`):

```bash
NC="${NIOBIUM_CLIENT_DIR:-/opt/niobium-client}"
./run-in-container.sh "cmake -S nb_out -B nb_out/build -DNIOBIUM_CLIENT_ROOT='$NC' \
    && cmake --build nb_out/build -j"
./run-in-container.sh "./run_test.sh --cpu"
```

## Documenting the run in the README

The application ships a run README that takes a newcomer from a fresh clone to a
run and back to a clean tree. It assumes the FHE-dev image (Docker on the host)
by default, or a local niobium-client build when the app was set up that way. Order it so the usage reads
end to end: obtain the image, run, tear down. Keep it about the application, not the
toolchain: per the attribution rule in SKILL.md, name the image, the `nb` DSL,
OpenFHE, or the Fog only where it helps a reader run, modify, or debug the app, not
as description or promotion. Beyond whatever the user asked for, it always includes:

- **Obtain the build-and-run environment.** Either the FHE-dev image (pull the
  published image, or build it from `environment/`), or a local niobium-client
  build on the host (`references/environment-setup.md` Path B), whichever the app
  was set up with. When the app is built locally, say so and record the
  `NIOBIUM_CLIENT_DIR` the run expects.
- **Inputs and outputs.** Enumerate and describe the data the application consumes
  and produces, as a table the reader can map to the code: each input feature (name,
  meaning, unit, and expected range or the bounds the client enforces) and each
  output field (name, meaning, unit, range). Include this **even when the data is
  synthesized**, because a reader cannot judge or reuse the application without
  knowing what the numbers are. For a classifier, list the classes and the base rate; for a
  regression, the target and its units. Name the parties concretely for this
  application (client = the user's side that holds the data and the key; server = the
  party that computes on it), not as abstract roles.
- **Regenerate any non-committed inputs.** The command(s) that rebuild anything not
  committed under `data/`.
- **DSL path: the generated `nb_out/`.** When the app is built with the `nb` DSL, note
  in the README that the committed `nb_out/` is a readable snapshot of the `nbc` output
  and is not pinned to a container version: the build regenerates it from the `.niob`
  sources with the image's `nbc`, so a change under `nb_out/` after a build is expected
  toolchain drift, and the `.niob` sources are the source of truth.
- **Run targets, led by the Fog.** A bare `run_test.sh` targets the Niobium Fog and
  is the default. List it **first** as the lead run command in the README (both
  where the run steps are given and in the run-modes table), with `--cpu` and
  `--sim` following as the local validation modes. Give each its command, its
  expected output, and its resource needs (peak server RSS, per-stage wall-clock,
  boundary sizes).
- **Client/server deployment.** The two-process run and its two-host variant (copy
  the server home to untrusted infrastructure; the secret key never leaves the
  client).
- **The error ledger, as a table.** Three rows: reference vs ground truth, twin vs
  reference, FHE vs twin. Attribute each residual to its actual source (model change,
  polynomial approximation, fixed-point quantization, encryption noise); do not fold
  quantization into the polynomial row.
- **Cleanup.** A `make clean` command that removes the build tree and every per-run
  artifact. State that `clean` lists its targets explicitly and never globs `run_*`,
  so it cannot delete `run_test.sh`. The committed inputs under `data/` (and, on the
  DSL path, the `.niob` sources and the generated `nb_out/`) survive, so a later run
  does not regenerate them. The same artifacts are ignored by the `.gitignore` the app
  ships, so a run leaves the working tree clean.

A recipient with Docker and the repository should need nothing else to reproduce the
run. Draft the commands and structure at the Stage 7 gate; fill the expected-output
blocks with the measured timings, peak RSS, and error after Stage 8.
