# Preparing the Build-and-Run Environment (Stage 0)

This reference is the detail behind **Stage 0**. It sets up one thing, once, so
you are never ambushed mid-design by a missing toolchain: a place where the
**faithful twin** and the **FHE program** can both be built and run.

The good news is that almost nothing has to be installed locally. Building
OpenFHE from source is the painful part of FHE development, and you skip it
entirely — you pull a prebuilt image instead.

## The mental model: two tiers, one data bus

The methodology runs at two speeds, and they have very different environment
needs. Setup mirrors that split.

**Twin tier (Stages 1–7) — runs where Claude runs.** The design work, the
parameter sweep, and the twin-vs-reference validation are pure Python (numpy).
Claude runs these in its own environment as you converse. You do **not** need
the container, or anything installed locally, for any of it. This is the bulk of
the work and all of the learning.

**FHE tier (Stage 8) — runs in the container, on your machine.** Building and
running the encrypted four-program OpenFHE app needs a full C++ + OpenFHE
toolchain, which is too heavy for Claude's sandbox. That is what the **FHE-dev
container** is for. It is the *only* place the container is required.

**The data bus is your project folder.** Claude writes source files into your
mounted project folder; the container is run with that same folder bind-mounted,
so it compiles that source and writes its outputs (decrypted results, logs)
right back into the folder, where Claude reads them. Files never have to be
copied by hand.

One correction to a natural assumption: the container does **not** run Claude or
any agent. It is a dumb build box. Claude is the brain and lives outside it; the
container only compiles and runs the code Claude writes.

## One-time setup

### 1. Install Docker (only if you don't have it)

Install [Docker Desktop](https://www.docker.com/products/docker-desktop/) (macOS
or Windows) or Docker Engine (Linux) and make sure it is running. This is the
single local install you cannot avoid — and it is far easier than building
OpenFHE.

Verify:

```bash
docker --version
```

### 2. Pull the FHE-dev image

```bash
docker pull ghcr.io/niobiuminc/fhe-dev:v0.7.0
```

This is a one-time download of a prebuilt OpenFHE + CMake + Python-ML
environment. You never build OpenFHE yourself.

### 3. Run the smoke test

Prove the environment can build and run OpenFHE C++ **and** run a numpy twin
before you invest in a design:

```bash
docker run --rm ghcr.io/niobiuminc/fhe-dev:v0.7.0 fhe-smoke-test
```

A successful run ends with `SMOKE OK` (after printing an `EvalAdd -> (5, 7, 9)`
line and a numpy result). If you see that, Stage 0 is complete and you can start
Stage 1.

The image also ships **`fhe-boot-lab`**, a bootstrap parameter lab used by
Stage 6's bootstrapping section: it measures REAL CKKS bootstrap accuracy at
candidate parameters in minutes (correctly forcing genuine refreshes —
EvalBootstrap silently no-ops on shallow inputs, so naive tests validate
nothing). Run it before designing any bootstrapped circuit:

```bash
docker run --rm ghcr.io/niobiuminc/fhe-dev:v0.7.0 fhe-boot-lab 50 51 24 16384 3 3 1
```

## How the container is used later (Stage 8)

You do not need to memorize any of this — at Stage 8 Claude writes the source
into your project folder and hands you a ready-to-paste command. It looks like:

```bash
# from your project folder
docker run --rm -v "$PWD":/work ghcr.io/niobiuminc/fhe-dev:v0.7.0 \
    bash -c "cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
             cmake --build build -j && ./build/run_test"
```

`-v "$PWD":/work` mounts your project folder into the container at `/work` (the
image's working directory), so the build sees Claude's source and its outputs
land back in your folder. Claude then reads those outputs and iterates. Because
the twin was already validated in Stage 7, this loop should converge in only a
few iterations.

## Which surface you drive

**Cowork (recommended, default).** Claude runs in a sandbox that cannot reach
your Mac's Docker, so *you* run the container commands — but you never write
them: Claude authors each `docker run` (usually a single one-shot build+run
command) and you paste it into your terminal, then report the result or let
Claude read the output files from the shared folder.

**Claude Code (optional, tight loop).** If you run Claude Code in a terminal on
the same machine, Claude's shell *is* your shell, so it runs the `docker run`
commands directly — a tighter compile/see-error/fix loop with no handoff. Same
image, same commands; the only difference is who presses enter.

## When the reference needs torch

If your Stage 3 **reference** is a PyTorch model, Claude's sandbox may not be
able to run it. The FHE-dev image bundles a CPU PyTorch, so you can produce the
reference's ground-truth outputs there too:

```bash
docker run --rm -v "$PWD":/work ghcr.io/niobiuminc/fhe-dev:v0.7.0 \
    python3 run_reference.py
```

## Troubleshooting

- **`docker: command not found`** — Docker isn't installed or not on PATH; see
  step 1.
- **`Cannot connect to the Docker daemon`** — Docker Desktop isn't running;
  start it and retry.
- **The smoke test fails to compile** — you likely have a stale image; re-pull
  (`docker pull ghcr.io/niobiuminc/fhe-dev:v0.7.0`).
- **Permission errors on the mounted folder (Linux)** — pass
  `--user "$(id -u):$(id -g)"` to `docker run` so container-written files are
  owned by you.
- **Image is large / slow first pull** — expected (it carries OpenFHE + a
  Python ML stack). It is a one-time cost; subsequent runs are instant.

## For maintainers: building and publishing the image

The image is defined in [`../environment/Dockerfile`](../environment/Dockerfile)
with [`../environment/smoke_test.sh`](../environment/smoke_test.sh).

Build it manually:

```bash
cd skills/fhe-application-design/environment
docker build -t ghcr.io/niobiuminc/fhe-dev:vX.Y.Z .
docker run --rm ghcr.io/niobiuminc/fhe-dev:vX.Y.Z fhe-smoke-test   # validate
docker push ghcr.io/niobiuminc/fhe-dev:vX.Y.Z                      # plus :latest
```

Or publish via CI: push an image tag (`git tag fhe-dev-v0.1.0 && git push origin
fhe-dev-v0.1.0`) to trigger `.github/workflows/publish-fhe-dev-image.yml`, which
builds, **runs the smoke test, and only then pushes** `:<version>` and `:latest`
to GHCR. Make the GHCR package public once (Packages → fhe-dev → Package
settings → Change visibility) so users can pull without authenticating.

Pin `OPENFHE_VERSION` (build arg) to the OpenFHE release the skill targets.
