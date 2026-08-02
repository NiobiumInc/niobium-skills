# Implementing the Design in the `nb` FHE DSL

This reference covers the **Stage 8 DSL implementation path**: turning the design
produced by Stages 1–6 into a working application using the `nb` domain-specific
language and its `nbc` cross-compiler, which live in the
[niobium-client](https://github.com/NiobiumInc/niobium-client) repository under
`dsl_fhe/`. The DSL compiles `.niob` source to OpenFHE C++ and generates
everything the raw-C++ track builds by hand: the four-program architecture,
serialization, CMake, key generation matched to the operations used, and
Niobium record/replay instrumentation.

The DSL is an alpha-stage tool and actively evolving. It covers the common CKKS
designs this skill targets; a few constructs are not yet available (see *What still
requires raw OpenFHE*) and the generated code is not yet fully optimized. The
limitations noted throughout are current-state on a maturing tool, not permanent
verdicts, and are worth working through.

Stage 8's contract in SKILL.md is **path-independent**: the same deliverables
(`run_test` and the run harness, the client/server two-process demo, client-side
input-bounds enforcement, twin validation, and Fog deployment) are required on this
path too. The [deliverable contract in DSL form](#the-stage-8-deliverable-contract-in-dsl-form)
below maps each one to its DSL realization; produce them all.

Authoritative language docs (read as needed; paths relative to `dsl_fhe/`):

| File | Content |
|---|---|
| `NB_LANGUAGE.md` | Language reference — types, syntax, built-in functions |
| `GRAMMAR.md` | Formal EBNF grammar |
| `HOWTO.md` | Step-by-step guide for adding a new example (Makefile targets, testing) |
| `CLAUDE.md` | Codegen internals and known pitfalls |

## How design outputs map to DSL constructs

Every artifact Stages 1–6 produce has a direct DSL counterpart. Use this table
to transcribe the design:

| Design output (stage) | DSL construct |
|---|---|
| Parties and trust boundaries (1) | `@client` / `@server` function annotations — compiled to separate binaries; server code referencing `SecretKey` or calling `decrypt()` is a **compile error** |
| What crosses the wire (1, 8) | `wire` type declarations — the compiler generates all serialization; a stage's `reads(...)`/`writes(...)` clauses are a machine-checked message-flow spec |
| Who encrypts / packing legality (1, 5) | Expressed in *which stage does the encrypting*, and **compiler-checked**: annotate independent-encryptor stages `@encryptors(independent)` and `encrypt()` of a column slice spanning records is a compile error (cross-owner SIMD packing). Single encryptor (default) → one `@client` stage packs records across slots, column-major |
| Scheme selection (4) | `scheme CKKS { ... }` block. **The DSL is CKKS-only today** — a BFV/BGV design must use raw OpenFHE |
| Depth budget (5, 6) | `depth:` in the scheme block; per-instance via `scheme.override(depth: inst.depth)`. The compiler **errors** when the statically tracked multiplication chain exceeds the budget (a sound lower bound) and warns on heavy over-provisioning. `chebyshev` with a literal/const `degree:` is a **modeled subcircuit**: it charges `ceil(log2(degree+1)) + 1` levels into the tracker (59 → 7, 119 → 8, …); non-literal degrees stay depth-opaque |
| Security ↔ parameters frontier (6) | Compile-time advisor: `nbc check` emits a `note:` computing `logQ ≈ first_mod + depth × q_i` and the minimum ring dimension for the declared security level (HE-standard tables), flagging declared `ring_dim`s below target (warning if no `scheme.override(security:)` dev profile covers them). Use it to trade q_i / depth / approximation degree against N |
| Ring dimension / slots (6) | `ring_dim` field on the `Instance` struct (applied automatically); `n_slots = ring_dim / 2` for CKKS |
| Security level (6) | `security:` in the scheme block; `scheme.override(security: not_set)` for toy/dev profiles |
| Scaling / first modulus (6) | `precision:` (scaling mod size) and `first_mod:` in the scheme block |
| Rotation keys (6) | `requires { add, mul, rotate, ... }` — keygen is generated to match; `slot_sum` uses EvalSum keys which are always generated |
| Column-major packing (5) | Load a row-major matrix with `load_matrix<f64>(path, cols)`, slice columns with `m[a..b, col]`, and `encrypt(pk, column)` — one ciphertext per feature/position |
| Comparison circuits (5) | Squared distance + iterated squaring: `diff * diff` accumulation + a squaring loop. Chebyshev: `chebyshev(|x| f(x), ct, domain: [lo, hi], degree: d)` |
| Slot aggregation (5) | `ct \|> slot_sum(n_slots)` (EvalSum, no depth cost) |
| Deferred relinearization (5) | `a *_norelin b` accumulation followed by `relin(acc)` |
| Four-program architecture (7) | One `@stage("name")` function per program — keygen / encrypt / server-compute / decrypt each becomes a standalone binary with CLI parsing, serialization, and (for `@server @hardware` stages) record/replay instrumentation |
| run_test (7) | Run the generated stage binaries in order, verifying against the plaintext reference (or a `test-<example>` Makefile target, if you contribute the app as an in-repo example) |
| Protocol spec + client/server demo (8, 9) | The domains, wire types, and `reads`/`writes` clauses document the message flow; you still write the threat-model prose and stand up the required two-process demo. See the deliverable contract below |

## The Stage 8 deliverable contract, in DSL form

Stage 8's deliverables are path-independent (SKILL.md). The DSL realizes each as
follows; produce all of them.

- **Trust boundary and negative test.** The `@client` / `@server` split is the
  primary guarantee: a `@server` stage that references `SecretKey` or calls
  `decrypt()` is a compile error, so the server binary cannot touch the secret key
  by construction. Also ship the runtime guard the contract asks for: provision the
  server home with no `sk.bin`, and have `run_test` assert its absence before
  launching the server. (The compile-time split is stronger than the OpenFHE runtime
  refusal for the leak property; the runtime guard is what a reviewer checks, so keep
  both.)
- **Client-side input-bounds enforcement.** The DSL sees only fixed-length numeric
  vectors, so enforce the Stage-5 bounds decision (reject or winsorize) in the
  `harness/` before `encrypt()`, and commit the bounds file. An out-of-domain input
  must be clipped or rejected there, never silently encrypted, the same requirement
  the OpenFHE `encrypt` program carries.
- **`run_test` and the run harness.** Generate the same `run-in-container.sh`,
  `run_test.sh` (the three modes with the Fog as the default target), and `Makefile`
  described in `run-harness.md`. The DSL gives you the comparison for free: for every
  twinnable stage the compiler emits a `<stage>_ref` cleartext twin, so `run_test`
  compares the encrypted run against `_ref` alongside the harness's plaintext
  reference. Build the generated `nb_out/` project, run the stage binaries in order,
  and report the same metrics: the application's own quality first, then FHE-vs-twin
  error, then the deployment profile. Wire `fhetch_sim` (`NBCC_FHETCH_SIM`,
  `LD_LIBRARY_PATH`) into the `--sim` mode.
- **Client/server two-process demo.** Required here too. The `@stage` binaries
  already run as separate processes exchanging serialized files, but stand them up as
  the contract specifies: two homes, the secret key only client-side, only ciphertext
  crossing, the server refusing a planted secret key, byte-count logging, and a
  server home safe to copy to an untrusted host.
- **Fog deployment.** A `@server @hardware(cache_key: [...])` stage compiles with the
  Niobium record/replay instrumentation built in, so the default `run_test` dispatch
  under `fog submit --target=FOG` works the same as the OpenFHE path, with no
  hand-written `niobium::compiler()` calls. The `--target` rules and the
  minimum-ring-dim guard in `niobium-client-fog-variant.md` apply unchanged.

## Workflow

1. **Plaintext preprocessing stays outside the DSL.** Anything string-shaped or
   data-wrangling (name encoding, Soundex, dataset generation, padding,
   computing the expected reference output) belongs in a small Python harness
   (a `harness/` directory in your project). The DSL sees only fixed-length
   numeric matrices/vectors, loaded with `load_matrix`/`load_vec`. The harness
   should also print/compute the **expected plaintext result** so the decrypt
   stage can verify against it — this is the Stage 3 "ground truth" discipline.
   The compiler reinforces it: for every stage free of non-twinnable
   constructs (`extern_call`, `replicate`, `running_sums`, `load_model`), it
   ALSO generates a `<stage>_ref` **cleartext reference twin** — the same
   `.niob` circuit with plaintext semantics (`enc<T>` → slot vectors, FHE ops →
   elementwise arithmetic, `chebyshev` → the true function). Run the `_ref`
   pipeline alongside the encrypted one; the difference between the two
   outputs is exactly the approximation + noise error.

2. **Write three files** in your project directory (the current working
   directory by default):
   - `shared.niob` — `Instance` struct (per-profile `ring_dim`, depth, sizes),
     directory-layout helpers, `wire` types.
   - `client.niob` — `scheme` block, `requires`, then `@client` stages:
     key generation, input encryption, decrypt-and-verify.
   - `server.niob` — `@server @stage(...) @hardware(cache_key: [...])` compute
     stage(s) plus shared circuit functions.

3. **Compile and build in your project directory.** From your project
   directory, run the cross-compiler against your `.niob` files, emitting the
   generated C++ into your project:
   ```
   python3 <niobium-client>/dsl_fhe/xcomp/nbc.py compile \
       shared.niob client.niob server.niob --outdir nb_out
   ```
   The generated `nb_out/` is a self-contained CMake project (depends only on
   OpenFHE): build it with `cmake -S nb_out -B nb_out/build && cmake --build
   nb_out/build`, then run the stage binaries in order and verify against the
   plaintext reference. (To contribute the app as a niobium-client example
   instead, add it under `dsl_fhe/examples/<name>/` with `make <name>` /
   `test-<name>` targets per `HOWTO.md`.)

4. **Iterate with the compiler's feedback**: run `nbc.py check shared.niob
   client.niob server.niob` from your project directory for parse/semantic
   checks; aim for `0 warnings, 0 errors`, then confirm numerics with the
   end-to-end run above.

5. **ML workloads: ground truth first, then sweep.** Require a full plaintext
   model implementation and a representative test set (Stage 3 — firm, not
   advisory). Then run the security-vs-accuracy sweep: vary (Chebyshev
   `degree:`, `precision:` (q_i), `depth:`, `ring_dim`) and measure
   end-to-end model accuracy at each point. The compiler's params note shows
   where each configuration sits on the security frontier; the generated
   `_ref` reference twins give each sweep point's accuracy (vs. the true
   nonlinearities) without running encryption; the encrypted pipeline
   confirms the final choice.

## Worked design↔implementation pairs

All three of this skill's worked examples have complete, tested DSL
implementations — read the design reference and the DSL code side by side:

| Design reference | DSL implementation (`dsl_fhe/examples/`) |
|---|---|
| `example-set-membership.md` | `set-membership/` — exact + Soundex profiles, squared distance + iterated squaring, ~180 lines of `.niob` |
| `example-fetch-by-similarity.md` | `fetch-by-similarity/` — Chebyshev threshold, slot replication, running sums, payload extraction |
| `example-network-intrusion-detection.md` | `fhe-NetworkMonitor/` — autoencoder ensemble, Chebyshev sigmoid/tanh, feature-major packing |

## Pitfalls specific to the DSL (current state)

1. **Encrypted-ness is fully structural — names carry no meaning.** The
   compiler classifies ciphertexts from annotations, builtin/user-fn return
   types, let-binding flow, loop elements, combinator-closure params, wire
   fields, and destructured tuples. There is no name heuristic: a variable
   the flow cannot resolve is treated as plaintext, and if it was actually a
   ciphertext the generated C++ fails to compile (never silently wrong). The
   fix is always an annotation (`let x: enc<vec<f64>> = ...`).

2. **Wire layouts are field-type-driven — names carry no special meaning.**
   Every wire serializes the same way: `enc<T>` → `{field}.bin`,
   `vec<enc<T>>` → `{field}_<i>.bin`, `vec<vec<enc<T>>>` →
   `batchNNNN/{field}_NNNN.bin`; a wire carrying a `CryptoContext` field (any
   name) gets the canonical `cc/pk/mk/rk` key layout; a `from:`/`to:` path
   naming a `.bin` file addresses a single-enc-field wire directly. Name your
   wires whatever fits the domain.

3. **One static `scheme` block per application.** Per-instance variation goes
   through `Instance` fields + `scheme.override(depth: ...)` /
   `scheme.override(security: not_set)` inside the keygen stage.

4. **Loops with runtime bounds defeat static depth tracking.** An iterated
   squaring loop `for r in 0..inst.k { t = t * t }` has data-dependent depth;
   the compiler's depth warnings are best-effort. Keep the Stage 5 depth
   budget arithmetic in a comment next to the `Instance` definition.

5. **`@hardware` cache keys gate record/replay.** A recorded trace is reused
   when the `cache_key` values match; if inputs change under the same key,
   delete the recorded program directory (`*_workload_*`,
   `fhetch_driver_source_*`) to force a fresh record.

6. **Generated keygen is not yet minimal.** As an alpha-stage tool, the codegen does
   not prune keys tightly yet: it may emit rotation / `EvalSum` (automorphism) keys
   the design's `requires { add, mul }` does not call for, which can substantially
   enlarge the server key bundle. This is a current codegen limitation under active
   improvement, not a design error. If server-bundle size matters, note it and keep
   the operation set (`requires { ... }`, `slot_sum` usage) as tight as the circuit
   allows.

## What still requires raw OpenFHE

For these, follow `implementing-with-openfhe.md` for the OpenFHE you write: a
whole-app switch (BFV/BGV, bootstrapping, and the rest of this list), or just the
hand-written C++ behind an `extern_call` when the rest of the app fits the DSL.
Reach for this once the DSL genuinely cannot express what you need, after the
toolchain and annotations are working. The generated `nb_out/` is OpenFHE too, so
this reference also helps you read or debug what the compiler produced. Do not
hand-edit `nb_out/` to work around the compiler, since it regenerates on recompile.

- **BFV / BGV** — the codegen targets CKKS only.
- **Transciphering** (Stage 5 output-integrity dual output) — no DSL
  construct yet; implementable via `extern_call` to hand-written C++ if the
  rest of the app fits the DSL.
- **Threshold / multi-party keys**, **bootstrapping**, **interactive
  multi-round protocols** — not expressible; the DSL's model is
  one-pass client → server → client.
