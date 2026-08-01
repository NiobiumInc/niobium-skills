# Preparing the Build-and-Run Environment (Stage 0)

This reference is the detail behind **Stage 0**. It sets up one thing, once, so
you are never ambushed mid-design by a missing toolchain: a place where the
**faithful twin** and the **FHE program** can both be built and run.

The good news is that almost nothing has to be installed locally. Building
OpenFHE from source is the painful part of FHE development, and you skip doing it
by hand — the FHE-dev image does it for you, built once from the skill's
Dockerfile.

## The mental model: two tiers, one data bus

The methodology runs at two speeds, and they have very different environment
needs. Setup mirrors that split.

**Twin tier (Stages 1–7) — runs where Claude runs.** The design work, the
parameter sweep, and the twin-vs-reference validation are pure Python (numpy).
Claude runs these in its own environment as you converse. You do **not** need
the container, or anything installed locally, for any of it. This is the bulk of
the work and all of the learning.

**FHE tier (Stages 8 and 10) — runs in the container, on your machine.** Building
and running the encrypted four-program OpenFHE app needs a full C++ + OpenFHE
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
single local install you cannot avoid.

Verify:

```bash
docker --version
```

### 2. Build the FHE-dev image

```bash
docker build -t ghcr.io/niobiuminc/fhe-dev:v0.13.0 skills/fhe-application-design/environment
```

The first build clones niobium-client and compiles the instrumented OpenFHE +
`libnbfhetch` from source; that is the one heavy step, so allow time for it.
Subsequent `docker run`s are instant. (The image builds niobium-client `main`;
pin a revision with `--build-arg GIT_REF=<sha>` for a reproducible image.)

### 3. Run the smoke test

Prove the environment can build and run OpenFHE C++ before you invest in a
design:

```bash
docker run --rm ghcr.io/niobiuminc/fhe-dev:v0.13.0 make test-release
```

It takes the bundled examples through record → simulate → decrypt; a green sweep
means Stage 0 is complete and you can start Stage 1.

The image also ships **`fhe-boot-lab`**, a bootstrap parameter lab used by
Stage 6's bootstrapping section: it measures REAL CKKS bootstrap accuracy at
candidate parameters in minutes (correctly forcing genuine refreshes —
EvalBootstrap silently no-ops on shallow inputs, so naive tests validate
nothing). Run it before designing any bootstrapped circuit:

```bash
docker run --rm ghcr.io/niobiuminc/fhe-dev:v0.13.0 fhe-boot-lab 50 51 24 16384 3 3 1
```

## How the container is used later (Stages 8 and 10)

You do not need to memorize any of this — at Stage 8 Claude writes the source
into your project folder along with a `run-in-container.sh` wrapper and a
`run_test.sh`, so the commands stay short. They look like:

```bash
./run-in-container.sh "cmake -S . -B build -DCMAKE_PREFIX_PATH='...' && cmake --build build -j"
./run-in-container.sh "./run_test.sh"          # no flag -> the Fog; --sim / --cpu validate locally
```

The wrapper mounts your project folder into the container at `/work` (and
`~/.fog` when present), so the build sees Claude's source and its outputs land
back in your folder. Claude then reads those outputs and iterates. Because the
twin was already validated in Stage 7, this loop should converge in only a few
iterations.

## Execution mode: self-run vs hand-off (probe, don't assume)

The container commands are identical no matter who runs them; the only variable
is **whether the agent can execute them in its own shell**. Detect that at
Stage 0 by trying the smoke test in the agent's shell, and behave accordingly —
this is a session *capability*, not a product name.

**Hand-off mode.** The agent's shell cannot reach Docker (the Cowork sandbox is
the common case, but any Docker-less environment qualifies). The agent authors
each command (usually a single one-shot build+run) and *you* paste it into your
terminal, then report the result or let the agent read the output files from the
shared folder. This is the careful one-command-at-a-time rhythm.

**Self-run mode.** The agent's shell *is* a Docker-capable shell (e.g. Claude
Code in a terminal on the same machine). The agent runs the `docker run` /
`run_test` / demo / Fog commands directly and iterates on build errors itself — a
tight compile/see-error/fix loop with no hand-off, surfacing results and
decisions rather than each command. It should not ask you to run or paste what it
can do itself.

Same image, same commands; the only difference is who presses enter. You can
override the detected mode in a sentence ("run it yourself" / "just give me the
commands"). Note a separate axis: even in self-run mode, a single step whose
resource needs exceed the local machine (e.g. a deep bootstrapped circuit) is
handed off *on capacity* to a bigger host or the compilation service — that is
about memory/compute, not about who can run Docker.

## When the reference needs torch

If your Stage 3 **reference** is a PyTorch model, Claude's sandbox may not be
able to run it. The FHE-dev image bundles a CPU PyTorch, so you can produce the
reference's ground-truth outputs there too:

```bash
docker run --rm -v "$PWD":/work -w /work ghcr.io/niobiuminc/fhe-dev:v0.13.0 \
    python3 run_reference.py
```

## Troubleshooting

- **`docker: command not found`** — Docker isn't installed or not on PATH; see
  step 1.
- **`Cannot connect to the Docker daemon`** — Docker Desktop isn't running;
  start it and retry.
- **A change to niobium-client isn't reflected** — the image caches the clone;
  rebuild with `--no-cache` (or `--build-arg GIT_REF=<sha>` to move the pin).
- **Permission errors on the mounted folder (Linux)** — pass
  `--user "$(id -u):$(id -g)"` to `docker run` so container-written files are
  owned by you.
- **The first build is large / slow** — expected (it compiles OpenFHE + carries a
  Python ML stack). It is a one-time cost; subsequent runs are instant.

## For maintainers: building and publishing the image

The image is defined in [`../environment/Dockerfile`](../environment/Dockerfile).
Build it manually:

```bash
docker build -t ghcr.io/niobiuminc/fhe-dev:vX.Y.Z skills/fhe-application-design/environment
docker run --rm ghcr.io/niobiuminc/fhe-dev:vX.Y.Z make test-release   # validate
```

This Dockerfile builds the image from within the skill.
