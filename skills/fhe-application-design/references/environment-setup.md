# Preparing the Build-and-Run Environment (Stage 0)

This reference is the detail behind **Stage 0**. It sets up one thing, once, so
you are never ambushed mid-design by a missing toolchain: a place where the
**faithful twin** and the **FHE program** can both be built and run.

Building OpenFHE from source is the painful part of FHE development. You get past
it one of two ways: pull the prebuilt FHE-dev image (nothing to compile, Docker is
the only local install), or build niobium-client from source once on the host (no
Docker, but a C++ toolchain). Either one gives you the same instrumented OpenFHE
and `libnbfhetch` to build and run against. This reference calls them **Path A**
(the image) and **Path B** (a local build).

## The mental model: two tiers, one data bus

The methodology runs at two speeds, and they have very different environment
needs. Setup mirrors that split.

**Twin tier (Stages 1–7) — runs where Claude runs.** The design work, the
parameter sweep, and the twin-vs-reference validation are pure Python (numpy).
Claude runs these in its own environment as you converse. You do **not** need
the container, or anything installed locally, for any of it. This is the bulk of
the work and all of the learning.

**FHE tier (Stages 8 and 10) — runs against the build environment, on your
machine.** Building and running the encrypted four-program OpenFHE app needs a
full C++ + OpenFHE toolchain, which is too heavy for Claude's sandbox. That is
the one tier that needs more than Python, and you provision it once, either as
the FHE-dev container (Path A) or a local niobium-client build (Path B).

**The data bus is your project folder.** In Path A, Claude writes source files
into your project folder; the container is run with that same folder
bind-mounted, so it compiles that source and writes its outputs (decrypted
results, logs) right back into the folder, where Claude reads them. Files never
have to be copied by hand. In Path B there is no mount: the app builds and runs
in place in the project folder, which already lives on the host.

One correction to a natural assumption: the container does **not** run Claude or
any agent. It is a dumb build box. Claude is the brain and lives outside it; the
container only compiles and runs the code Claude writes.

## One-time setup

You provision the build environment once, up front, one of two ways. Do this
before Stage 1 and treat the smoke test as the gate.

- **Path A — the FHE-dev image (recommended).** Needs only Docker.
- **Path B — a local niobium-client build.** Needs a C++ toolchain, no Docker.

Both give the same instrumented OpenFHE and `libnbfhetch`, and everything
downstream (the run harness, the run modes, the later commands) is identical.

**The path is the user's choice, so ask for it** (SKILL.md Stage 0 carries the
question wording). Search for an existing niobium-client installation first and
tell the user that is what you are looking for, so the question carries the result:

```bash
# an existing pointer
echo "${NIOBIUM_CLIENT_DIR:-<unset>}"
# a checkout this skill is installed under, the starter kit's vendored submodule,
# or a sibling clone
ls -d ../niobium-client-fog-starter-kit/niobium-client ../niobium-client 2>/dev/null
# built already? the OpenFHE path needs the installed Config file
ls <checkout>/vendor/lib/niobium-client/lib/cmake/NiobiumFhetch/NiobiumFhetchConfig.cmake 2>/dev/null
```

Recommend Path A when the search finds nothing. When it finds a built checkout,
report the path and note that Path B builds against it without the source build,
then let the user decide. Take Path B without asking only when Docker is absent
and cannot be installed.

## Path A: FHE-dev image

### 1. Install Docker (only if you don't have it)

Install [Docker Desktop](https://www.docker.com/products/docker-desktop/) (macOS
or Windows) or Docker Engine (Linux) and make sure it is running. On this path
Docker is the only local install.

Verify:

```bash
docker --version
```

### 2. Get the FHE-dev image

Pull the prebuilt image from the GitHub Container Registry, or build it from the
skill's `environment/` directory. That path depends on where the skill is
installed: `.claude/skills/fhe-application-design/environment` (Claude),
`.agents/skills/fhe-application-design/environment` (Codex and other
agentskills.io agents), or `skills/fhe-application-design/environment` from a clone
of the skill repo.

```bash
# Pull the prebuilt image from ghcr:
docker pull ghcr.io/niobiuminc/fhe-dev:latest

# Or build it from the skill's environment/ directory:
docker build -t ghcr.io/niobiuminc/fhe-dev:latest skills/fhe-application-design/environment
```

The first build clones niobium-client and compiles the instrumented OpenFHE +
`libnbfhetch` from source; that is the one heavy step, so allow time for it.
Subsequent `docker run`s are instant. (The image builds niobium-client `main`;
pin a revision with `--build-arg GIT_REF=<sha>` for a reproducible image.)

### 3. Run the smoke test

Prove the environment can build and run OpenFHE C++ before you invest in a
design:

```bash
docker run --rm ghcr.io/niobiuminc/fhe-dev:latest make test-release
```

It takes the bundled examples through record → simulate → decrypt; a green sweep
means Stage 0 is complete and you can start Stage 1.

The image also ships **`fhe-boot-lab`**, a bootstrap parameter lab used by
Stage 6's bootstrapping section: it measures REAL CKKS bootstrap accuracy at
candidate parameters in minutes (correctly forcing genuine refreshes —
EvalBootstrap silently no-ops on shallow inputs, so naive tests validate
nothing). Run it before designing any bootstrapped circuit:

```bash
docker run --rm ghcr.io/niobiuminc/fhe-dev:latest fhe-boot-lab 50 51 24 16384 3 3 1
```

## Path B: local niobium-client build

Build niobium-client from source once on the host, then point the skill at the
checkout. No Docker; a C++ toolchain does the work instead.

### 1. Install build prerequisites

A C++17 compiler, CMake 3.16+, OpenSSL 3, and Python 3.

macOS (Apple ships LibreSSL, so point CMake at Homebrew's OpenSSL):

```bash
xcode-select --install
brew install cmake openssl@3 python3
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
```

Linux (Debian / Ubuntu):

```bash
sudo apt-get update && sudo apt-get install -y build-essential cmake libssl-dev python3 git
```

### 2. Get niobium-client and build it

Acquire the client, then build it. Three ways to acquire it:

- **Already have a checkout** (you are working inside one, or the Niobium Fog
  starter kit beside the project vendors it as a submodule at
  `../niobium-client-fog-starter-kit/niobium-client/`): use it as is and skip to
  the build. For the OpenFHE
  path, confirm it was installed with `make install-release` (its
  `vendor/lib/niobium-client/lib/cmake/NiobiumFhetch/` holds
  `NiobiumFhetchConfig.cmake`); if only `NiobiumFhetchTargets.cmake` is there, run
  `make install-release` in it.
- **A submodule of your project (default).** When the skill is installed in a git
  repo, add the client as a submodule so its version travels with the app, and
  commit the gitlink and `.gitmodules` so the pin is recorded (an uncommitted pin
  does not survive a fresh clone, and some build scripts reset it):
  ```bash
  git submodule add https://github.com/NiobiumInc/niobium-client.git niobium-client
  git add .gitmodules niobium-client && git commit -m "Vendor niobium-client"
  cd niobium-client
  ```
- **A shared standalone clone.** To build the heavy client once and reuse it across
  several apps (or when the project is not a git repo), clone it somewhere central
  instead: `git clone https://github.com/NiobiumInc/niobium-client.git && cd niobium-client`.

Then build and put the `fog` CLI on PATH. Fetch only the niobium-fhetch submodule
and its nested OpenFHE, skipping the GPU-oriented niobium-haze exactly as the image
does:

```bash
make sync-fhetch     # niobium-fhetch + nested OpenFHE only (no niobium-haze)
make release         # build the instrumented OpenFHE + libnbfhetch (Release)
make install-release # install them + NiobiumFhetchConfig.cmake under vendor/lib/ (find_package needs this)
make install-cli     # install fog + nbcc_fhetch_replay to ~/.local/bin
```

The install is checkout-relative and the same on macOS and Linux. `make release`
builds the instrumented OpenFHE and `libnbfhetch`; **`make install-release`
installs them** under `<checkout>/vendor/lib/openfhe` and
`<checkout>/vendor/lib/niobium-client`, including `NiobiumFhetchConfig.cmake` so the
app's `find_package(NiobiumFhetch)` resolves. Skipping it leaves only
`vendor/lib/openfhe`, and the build cannot find the SDK. `make install-cli` puts
`fog` and `nbcc_fhetch_replay` in `~/.local/bin` (override with `CLI_PREFIX=`).
Build the app against this same OpenFHE only: mixing another
OpenFHE (system or Homebrew) with the client's `libnbfhetch` fails at link with
`undefined reference to lbcrypto::...`. The generated build points there already
(the `CMAKE_PREFIX_PATH` in `run-harness.md`). Put the bin directory on PATH
(`~/.zshrc` on macOS, `~/.bashrc` on Linux):

```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc && exec $SHELL
```

### 3. Point the skill at the checkout

Export `NIOBIUM_CLIENT_DIR` to the checkout's absolute path. The generated
`run.sh` reads it and runs the build and every run mode directly on
the host instead of in a container, and the build's `CMAKE_PREFIX_PATH` resolves
against it:

```bash
export NIOBIUM_CLIENT_DIR="$PWD"     # run from inside the checkout
```

The DSL compiler needs no install: `nbc` runs in place from
`$NIOBIUM_CLIENT_DIR/dsl_fhe/xcomp/nbc.py`.

### 4. Run the smoke test

Prove the toolchain can build and run OpenFHE C++ before you invest in a design.
Run it natively from inside the checkout:

```bash
make test-release
```

It takes the bundled examples through record, simulate, and decrypt; a green
sweep means Stage 0 is complete and you can start Stage 1.

**Bootstrap lab (Stage 6), only if your design bootstraps.** `make release` does
not build `fhe-boot-lab`. Compile it once against the local install from the
skill's `environment/boot_lab.cpp` (the same source the image bakes in):

```bash
NC="$NIOBIUM_CLIENT_DIR"
# boot_lab.cpp ships in the installed skill's environment/ directory. Set SKILL_ENV
# to <skill>/environment, where <skill> is .claude/skills/fhe-application-design
# (Claude), .agents/skills/fhe-application-design (Codex and other agentskills.io
# agents), or skills/fhe-application-design from a clone of the skill repo.
SKILL_ENV="<skill>/environment"
g++ -O2 -std=c++17 "$SKILL_ENV/boot_lab.cpp" -o ~/.local/bin/fhe-boot-lab \
    -I"$NC/vendor/lib/openfhe/include/openfhe" \
    -I"$NC/vendor/lib/openfhe/include/openfhe/core" \
    -I"$NC/vendor/lib/openfhe/include/openfhe/pke" \
    -I"$NC/vendor/lib/openfhe/include/openfhe/binfhe" \
    -I"$NC/vendor/lib/openfhe/include/openfhe/third-party/include" \
    -L"$NC/vendor/lib/openfhe/lib" -lOPENFHEpke -lOPENFHEcore -lOPENFHEbinfhe \
    -L"$NC/vendor/lib/niobium-client/lib" -lnbfhetch \
    -Wl,-rpath,"$NC/vendor/lib/openfhe/lib" -Wl,-rpath,"$NC/vendor/lib/niobium-client/lib"
```

## How the build environment is used later (Stages 8 and 10)

You do not need to memorize any of this — at Stage 8 Claude writes the source
into your project folder along with a `run.sh` wrapper and a
`run_test.sh`, so the commands stay short. They look like:

```bash
./run.sh "cmake -S . -B build -DCMAKE_PREFIX_PATH='...' && cmake --build build -j"
./run.sh "./run_test.sh"          # no flag -> the Fog; --sim / --cpu validate locally
```

In Path A the wrapper mounts your project folder into the container at `/work`
(and `~/.fog` when present), so the build sees Claude's source and its outputs
land back in your folder. In Path B the same wrapper runs the same commands in
place on the host (its `MODE_DEFAULT`, `--local`, or an exported `NIOBIUM_CLIENT_DIR`
selects the host and skips the container), so the
call sites above do not change. Claude then reads those outputs and iterates.
Because the twin was already validated in Stage 7, this loop should converge in
only a few iterations.

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
docker run --rm -v "$PWD":/work -w /work ghcr.io/niobiuminc/fhe-dev:latest \
    python3 run_reference.py
```

In Path B run it with the host's Python (`python3 run_reference.py`), installing
`torch` there if it is missing, since the local build does not carry a Python ML
stack.

## Troubleshooting

Path A (image):

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

Path B (local build):

- **macOS `make release` fails at `find_package(OpenSSL)`, or no "TLS enabled"
  line** — Apple ships LibreSSL; `brew install openssl@3` and
  `export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`, then rebuild. Without
  TLS the build finishes but `fog submit` cannot reach the Niobium Fog.
- **`fog: command not found`** — `~/.local/bin` is not on PATH; add it (step 2)
  or re-run `make install-cli` with a `CLI_PREFIX=` that is.
- **`run.sh` still uses Docker** — the app was generated with `MODE_DEFAULT=container`
  and `NIOBIUM_CLIENT_DIR` is not exported in the shell running it. Export it to the
  checkout's absolute path, pass `--local` for a single call, or set `MODE_DEFAULT=local`
  in the script to change the app's default.
- **The app build cannot find NiobiumFhetch or OpenFHE** — `make release` has not
  installed under `<checkout>/vendor/lib/`, or `NIOBIUM_CLIENT_DIR` points at the
  wrong directory; confirm both, then rebuild.

## For maintainers: building and publishing the image

The image is defined in [`../environment/Dockerfile`](../environment/Dockerfile).
Build it manually:

```bash
docker build -t ghcr.io/niobiuminc/fhe-dev:vX.Y.Z skills/fhe-application-design/environment
docker run --rm ghcr.io/niobiuminc/fhe-dev:vX.Y.Z make test-release   # validate
```

This Dockerfile builds the image from within the skill.
