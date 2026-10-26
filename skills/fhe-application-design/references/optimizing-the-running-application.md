# Optimizing the running application (Stage 11)

This reference is the detail behind **Stage 11**. It assumes the application
already works end to end: the Fog (or simulator) run reproduces the faithful twin
within the Stage 7 tolerance, the two-process demo stands, and the Stage 9
documents are closed out. Nothing here is a correctness fix. Every item trades
something the user can see (bytes on the wire, seconds, gigabytes on the server)
for something the design has to pay (depth, server work, a new key, a new
dependency), and the point of this reference is to make each trade legible and
each result re-verified.

Contents:
- [The discipline: measure, change one thing, re-validate](#the-discipline)
- [Where the bytes are (a sizing model)](#where-the-bytes-are)
- [A. Shrinking the key material](#a-shrinking-the-key-material)
- [B. Shrinking the per-request upload](#b-shrinking-the-per-request-upload)
- [C. Shrinking the per-request download](#c-shrinking-the-per-request-download)
- [D. Serialization and transport](#d-serialization-and-transport)
- [E. Compute-side optimizations](#e-compute-side-optimizations)
- [The candidate table and the choice](#the-candidate-table-and-the-choice)
- [The optimization ledger](#the-optimization-ledger)
- [Explaining the trades to the user](#explaining-the-trades-to-the-user)

## The discipline

Three rules keep an optimization pass from turning a working application into a
broken one.

**Measure before choosing.** `run_test` already prints the boundary sizes
(keys, input ciphertexts, output ciphertexts), per-stage timings, and peak server
memory. Those numbers are the baseline ledger. Read them, find the largest cost
that matters to this deployment (a one-time key transfer is a different problem
from a per-request upload), and attack that. Do not start from the list of
techniques; start from the measurement. Many optimizations shrink a number that
was never the bottleneck.

**Change one thing, behind a switch.** Implement each optimization so the
unoptimized path stays runnable (a flag on `encrypt`/`server`, or a build option)
until the optimized path has been validated. Two reasons: the baseline is the
comparison for the before/after numbers, and when an optimized run fails you
need to know whether the optimization or something else broke it. Land one
optimization, re-validate, record it, then start the next.

**Re-validate to the same bar, every time.** An optimization that changes what
crosses the wire, at what level, or under what encryption changes the noise the
circuit sees. So each one re-runs the full gate: `run_test --cpu`, `--sim`, and
the Fog run, each reproducing the twin within the recorded tolerance with zero
decision flips, plus the two-process demo (the server home still contains no
secret key; whatever new material the optimization sends to the server is
listed and justified). Any optimization that fails this gate is reverted, not
tuned around.

## Where the bytes are

At ring dimension N = 2^16 with 64-bit RNS limbs, one ciphertext **limb pair**
(both polynomials, one prime) is 2 × 65,536 × 8 B = **1 MiB**. Everything below
is a multiple of that:

| object | size | notes |
|---|---|---|
| fresh ciphertext, depth d | (d + 1) MiB | depth 12 → 13 MiB; depth 25 → 26 MiB |
| ciphertext at 1 limb | 1 MiB | the floor for anything decryptable |
| public key | (d + 1) MiB | same shape as a fresh ciphertext |
| one evaluation key (relin or one rotation) | ≈ dnum × (d + 1) MiB | dnum 3, depth 12 → ≈ 40 MiB |
| bootstrapping keys | many rotation keys | commonly hundreds of MiB to GiB; the dominant server-side key cost |
| the plaintext behind one full ciphertext | 32,768 × 8 B = 256 KiB | what a 13 MiB input actually carries |

So a batched application that ships one ciphertext per feature at depth 12 is
sending ~50× the plaintext size per feature column, and a rotation-heavy circuit
can ship more key material at setup than it will ship ciphertext in a week of
requests. Which of those matters is the deployment's call, which is why the pass
starts from the measurement.

## A. Shrinking the key material

Key material is a **setup** cost: it ships once per key pair and is reused for
every request. Optimize it when setup bandwidth or server key memory is the
constraint, or when keys rotate often (Stage 1 question 3).

### A1. Ship only the keys the circuit uses

Already the rule from Stage 6, restated because it is the cheapest win: generate
rotation keys for exactly the rotation amounts the circuit performs, not for
every power of two. A column-major linear layer needs none. Confirm by reading
the key list `keygen` writes against the rotations the server calls.

### A2. Seed-expanded evaluation keys

Each evaluation key is a set of RLWE samples of the form (b, a), where every `a`
is a uniformly random polynomial that is public by construction. A deterministic
cryptographic PRNG can regenerate each `a` from a short seed, so the client can
serialize **only the `b` halves plus one seed** and the server expands the `a`
halves on arrival. This halves every evaluation key, the public key, and
(see B3) a secret-key-encrypted ciphertext.

**This does not need library support, but it is not only a PRNG.** Mainline
OpenFHE does not expand keys from a seed (issue
openfheorg/openfhe-development#978 is open and milestoned for a later release)
and niobium-client does not add it, so check the installed SDK for a seeded API
first. Without one, the application can still do it, with one consequence that
shapes the whole design: the library samples `a` *inside* key generation and
derives `b` from it, and there is no hook to hand in your own `a`. So the server
can only regenerate what the client chose, and the client can only choose it if
the client computed `b` against it. **The application must generate every seeded
object itself**, in `keygen`, from public API; reconstruction on the server is
the easy half. Per object:

- *Public key.* Sample `a` from the application's seeded expander over the
  public-key parameters, sample `e` from the library's discrete Gaussian, set
  `b = −a·s + e` (the secret key exposes its polynomial and `DCRTPoly` has the
  arithmetic), and install both with `SetPublicElements`.
- *Relinearization and rotation keys.* The real work. A hybrid key-switching key
  is a vector of (`b`, `a`) pairs, one per digit, over the **extended modulus
  Q·P**, not over Q: the expander must produce limbs for every prime in both
  chains, and each digit's `b` carries the digit-decomposed old key scaled by P.
  The formula is a few dozen lines in the library's hybrid key-switch
  generator; transcribe it faithfully and install the vectors with
  `SetAVector` / `SetBVector` on the key object. Budget a few hundred lines
  including tests.
- *Server side.* Deserialize the `b` halves and the seed, re-run the same
  expander, install the `a` halves with the same setters, and use the key as
  usual.

Seven pitfalls, each of which has turned a "simple" seeded-key implementation
into either a leak or silent garbage:

1. **Never seed the library's own PRNG.** OpenFHE has one global engine that
   also draws the secret key and every error term; seeding it with a public
   seed would reveal `s` and `e`. The expander for `a` is a *separate* PRNG
   that touches nothing else. The library's Blake2 engine class can be
   instantiated with an explicit seed and counter and documents that the same
   seed and counter reproduce the same stream, which makes it a reasonable
   choice for the `a` stream specifically.
2. **Uniform modulo each prime, without bias.** Each limb is a different
   ~60-bit prime. Reduce random words by rejection sampling (or a wider
   reduction), and fix the limb order and coefficient order once. Both ends
   must produce identical words, so the expansion procedure is part of the
   wire format, not an implementation detail.
3. **Sample directly in evaluation (NTT) form.** The library samples `a` in
   NTT form; uniform in NTT form is the same distribution as uniform in
   coefficient form. Doing the same avoids a transform and one more thing to
   disagree about.
4. **Key generation stays in the client home.** It now runs in application
   code, so it belongs in `keygen` under the same no-secret-key guard the
   server enforces; the server program never gains a code path that touches
   `s`.
5. **Validate identity, then function, then the gate.** Hash the expanded `a`
   vectors on the server and compare against the client's hash before use. Add
   a unit test that a library-produced ciphertext key-switches and decrypts
   correctly under the application-built keys. Only then run the normal Stage
   11 gate. A wire-format mismatch produces garbage, not an error.
6. **Pin the library version.** The key-switching formula tracks the hybrid
   key-switch implementation; re-run the identity test on every SDK upgrade.
7. **Expand before the trace.** The Fog trace carries evaluation keys, so the
   safe default is to expand on the CPU server before recording; whether the
   Fog can consume seeded keys directly is a question for the Niobium client.

- **Saves:** ~50% of all key material and of the public key.
- **Costs:** application-owned generation of the seeded objects (small for the
  public key, a few hundred lines plus tests for key-switching keys); one PRNG
  expansion per key on the server at load time (fast, once); a documented
  expansion procedure as part of the wire format.
- **Needs:** either a seeded API in the installed SDK, or the application-owned
  generation above. Nothing from the library beyond its public setters and
  polynomial arithmetic.
- **Security:** none lost. The `a` halves are public already; the seed is
  public too. The expander must be a cryptographic PRNG, separate from the
  library's (pitfall 1), and the seed fresh per key generation.
- **Re-validate:** the identity hash and the key-switch unit test from
  pitfall 5, then the normal gate. `--sim-full`'s ring-level identity check
  confirms the expanded keys drive the same computation on both paths.

### A3. Fewer, larger key-switching digits

`dnum` is a direct dial on evaluation-key size: fewer digits means smaller keys
at the cost of a larger special modulus (which eats into the security ceiling)
and more noise per key switch. `references/performance-and-packing.md` §4
covers the trade; the Stage 0 hardware envelope caps what fits. Treat it as a
parameter change, which means re-running the Stage 6 decode-safety arithmetic
before the gate.

### A4. Hierarchical rotation keys (the server derives most of them)

A2 halves each rotation key; this halves nothing per key but cuts **how many
keys the client has to generate and send at all**. Lee, Lee, Kim and No,
*Rotation Key Reduction for Client-Server Systems of Deep Neural Network on
Fully Homomorphic Encryption* (IACR ePrint 2022/532,
https://eprint.iacr.org/2022/532), make rotation keys hierarchical: the client
generates and ships only a small **base set** of rotation keys, and the server
derives every other rotation key the circuit needs from the public key and
that base set, once, at setup. The derivation is public-key work on public
material, so no secret leaves the client. Reported on ResNet-18 under CKKS: the
full key set drops from 115.7 GB to 2.91 GB with a two-level hierarchy (client
key generation from 145 s to 3.7 s) and to 1.54 GB with three levels (1.9 s).
The two-level variant leaves the homomorphic operations on ciphertexts
unchanged; the three-level variant adds some key-switching work.

This is the lever for circuits whose rotation-key set is measured in
gigabytes: BSGS matmuls over many diagonals, deep networks, and **bootstrapping
key sets**, which are rotation keys and therefore derivable the same way. For a
column-major MLP with no rotations there is nothing to derive, and the row
reads "no gain here".

- **Saves:** the client's key generation time and the setup transfer, by one
  to two orders of magnitude on rotation-heavy circuits; composes with A2
  (seed-expand the base set) since the two attack different factors of the
  same product.
- **Costs:** a one-time derivation pass on the server per key pair (key
  switching, offline); **derived keys carry the noise of their derivation**,
  so a circuit that was noise-marginal re-runs the Stage 6 decode-safety
  arithmetic with the derived keys' noise in the margin; the three-level
  variant also adds key-switching operations per use. Server key memory does
  not shrink: the derived keys still exist on the server, so this helps
  bandwidth and client time, not the device-memory envelope.
- **Needs:** a build. Neither mainline OpenFHE nor niobium-client ships
  hierarchical key derivation as of this writing. The server-side derivation
  follows the paper's construction and installs the derived keys with the same
  setters A2 uses; the client ships the base set and a manifest of which
  rotations the server must derive. Check with Niobium whether the Fog path
  can accept server-derived keys before building.
- **Security:** none lost in the standard model; the base keys and the public
  key are already public, and the derivation uses nothing else. Document in
  Stage 9 that the server now holds a key-derivation step.
- **Re-validate:** a unit test that a ciphertext rotated under a derived key
  equals the same rotation under a client-generated key within noise (this
  bounds the derivation noise directly), then the normal gate on the worst-case
  slot population, since the extra noise is where a marginal decode tips.

## B. Shrinking the per-request upload

The per-request upload is the cost that scales with usage, so it is usually the
one worth the most engineering. The options below are ordered from free to
project-sized.

### B1. Level-matched inputs (free; do this first)

A fresh ciphertext carries every limb in the context, but the circuit only needs
as many levels as it will consume before its first refresh (or before it ends).
In a tight non-bootstrapped design those are the same number and there is
nothing to gain. In a **bootstrapped** design they are very different: the
context may hold 25–30 limbs so that bootstrapping fits, while the first segment
consumes 8. Encode each input at the level the first segment actually starts
from, and the input ships with 9 limbs instead of 26.

In OpenFHE the encoder takes the level directly:
`cc->MakeCKKSPackedPlaintext(values, /*noiseScaleDeg=*/1, /*level=*/L)`; the
ciphertext encrypted from that plaintext has `depth − L` levels left. The server
circuit is unchanged; it already expected its input at that level if the
segmentation from Stage 6 was honored.

- **Saves:** (26 − 9)/26 ≈ 65% of the upload in the example above; nothing in a
  tight design.
- **Costs:** nothing at run time. One design-time check: the level must leave at
  least the first segment's depth plus the one-level pessimism margin from
  Stage 6, or the first refresh fires early (or the circuit overflows).
- **Needs:** nothing new.
- **Re-validate:** the normal gate. Compare the first refresh's measured level
  against the plan.

### B2. Fully depleted inputs, bootstrapped on arrival

Push B1 to its limit: ship every input at **one or two limbs** and have the
server bootstrap each input ciphertext before the circuit runs. The upload drops
to ~1–2 MiB per ciphertext regardless of circuit depth.

- **Saves:** (d + 1) MiB → 1–2 MiB per input ciphertext; at depth 25 that is
  ~92%.
- **Costs:** one bootstrap per input ciphertext per request (seconds to tens of
  seconds each on CPU; the Fog's bootstrapping throughput is what makes this
  practical), **bootstrapping keys on the server** (hundreds of MiB to GiB, once,
  which can erase the saving for a server that was not already bootstrapping),
  and the bootstrap's own noise and constraints from Stage 6: the input
  magnitude must be ≲ 1 (fold input scaling into the first plaintext operand),
  the refresh error is a measured quantity (run `fhe-boot-lab` at the exact
  parameters), and `first − scaling` must respect the correction factor.
- **Needs:** bootstrapping support on the deployment target (the Stage 0
  profile table), a context configured for bootstrapping (`FHE` feature,
  `EvalBootstrapSetup`, `EvalBootstrapKeyGen`), and SPARSE_TERNARY or another
  secret distribution the lab has shown clean at your scaling.
- **Security:** unchanged.
- **Re-validate:** the twin now needs a per-input refresh noise term (the Stage 7
  tolerance was derived without one), then the normal gate. Confirm with the
  instrumented-decrypt debug mode that the post-bootstrap input matches the
  plaintext within the lab's measured error before trusting the end-to-end
  number.

Use B2 only when the server already bootstraps (the keys are then free) or when
the per-request upload is the binding constraint and the one-time key transfer
is not. For a design that never bootstraps, B1 and B4 are the honest options.

### B3. Secret-key encryption of inputs

The client holds the secret key, so it can encrypt inputs with it instead of
with the public key. A secret-key encryption is (−a·s + e + m, a) with `a`
uniformly random, which means it is exactly the shape A2 compresses: with seed
expansion, the client ships **one polynomial plus a seed** per ciphertext, half
the size.

As with A2, the library's `cc->Encrypt(keys.secretKey, pt)` samples its own `a`,
so the application builds the seeded ciphertext itself, and this is the easiest
of the seeded objects. The reliable recipe: encrypt the plaintext once normally
to get a **template** ciphertext with correct metadata (level, noise scale
degree, scaling factor, slots), then replace its two elements with `a` from the
seeded expander (one seed per ciphertext, or one seed plus a per-ciphertext
counter) and `b = −a·s + e + m`, where `m` is the encoded plaintext's polynomial
and `e` is fresh library Gaussian noise. The ciphertext class exposes
`SetElements` and setters for every metadata field. The server rebuilds `a`
from the seed, installs it, and proceeds. All seven A2 pitfalls apply,
especially the first: the expander is never the library's global PRNG, which
also draws `e`.

- **Saves:** 50% of the upload, but **only together with A2's expander**;
  without seed expansion both halves still ship and nothing is gained.
- **Costs:** the application-built encryption path (small, given the template
  trick) and a per-ciphertext seed or counter on the wire. The `encrypt`
  program must now hold the secret key. In the four-program split it already
  lives in the client home beside `decrypt`, so the trust boundary is
  unchanged, but the Stage 9 document must say that `encrypt` is a
  secret-key-bearing program.
- **Needs:** the A2 expander and wire format.
- **Re-validate:** the identity hash on the server, a unit test that the
  application-built ciphertext decrypts to the plaintext within fresh-encryption
  noise, then the normal gate. The fresh noise of a secret-key encryption is
  smaller than a public-key one, so nothing tightens.

### B4. Hybrid FHE: symmetric input transciphering

The largest reduction available, and the largest project. The client encrypts
its inputs with an **FHE-friendly symmetric cipher** under a short symmetric key
K, which costs about plaintext size (ciphertext expansion of 1.2–1.5× for the
CKKS-oriented ciphers), and ships those compact ciphertexts plus **one FHE
ciphertext of K** per session. The server homomorphically evaluates the
cipher's decryption, turning the symmetric ciphertexts into ordinary FHE
ciphertexts of the inputs, and then runs the circuit as before. The upload for a
32,768-value column drops from (d + 1) MiB to roughly 300–400 KiB, a 30–60×
reduction at typical depths.

This is the input-side mirror of the output-integrity transciphering already in
Stage 5, with the same family of ciphers:

| cipher | designed for | decryption depth | notes |
|---|---|---|---|
| Rubato | CKKS via the RtF framework | 2–5 levels (block size 16–64) | expansion ≈ 1.26–1.31; the usual CKKS choice |
| HERA | CKKS via the RtF framework | ≈ 4–5 levels | expansion ≈ 1.23–1.54 |
| Pasta | BFV/BGV (integer) | ≈ 5 levels | integer scheme only; not a CKKS cipher |

- **Saves:** 95–98% of the per-request upload. The per-session key ciphertext is
  one ordinary ciphertext.
- **Costs:** the cipher's decryption depth is added to the circuit's depth budget
  **in front of** the application circuit, so the Stage 6 parameter sizing moves
  (a 12-level circuit with a 5-level cipher is a 17-level design, with everything
  that implies for logQ and N); the server does extra work per request (the
  decryption circuit, which for RtF-style ciphers on CKKS involves an integer
  evaluation layer and a real-to-finite-field conversion, not a plain CKKS
  polynomial); and the client gains a symmetric-cipher implementation with a
  nonce/IV discipline (a keystream must never be reused across requests).
- **Needs:** an implementation. As of this writing neither mainline OpenFHE nor
  niobium-client ships a transciphering layer, so this is a build, not a
  configuration: the cipher's reference code on the client side and its
  homomorphic decryption circuit on the server side, both validated against each
  other in plaintext first (a twin of the cipher) before they meet the
  application circuit. Budget it as a sub-project with its own Stage 7-style
  gate. Check with Niobium whether a supported implementation exists for the
  target before building one.
- **Security:** the symmetric key travels only under FHE; the symmetric
  ciphertexts are as strong as the cipher at its parameter set (use the
  published 128-bit parameters). Document the nonce scheme in Stage 9. If the
  application also uses output-integrity transciphering, keep the two keys and
  nonce spaces separate.
- **Re-validate:** the transciphering layer is exact arithmetic in principle but
  it consumes levels and adds noise, so the twin must model it (or at least its
  noise and level consumption) before the Stage 7 tolerance is reused. Then the
  normal gate, plus a direct check that the server's decrypted-under-FHE inputs
  match the client's plaintext inputs within noise.

Choose B4 when the per-request upload is the dominant, recurring cost and the
depth budget has room for the cipher. Choose B2 when the server already
bootstraps. Choose B1 always.

## C. Shrinking the per-request download

The result ciphertext is already smaller than the inputs because the circuit
consumed levels, but it is rarely at the floor.

### C1. Compress the result to one limb before returning it

OpenFHE's `cc->Compress(ct, /*towersLeft=*/1)` drops every limb the decryptor
does not need, leaving a 1 MiB ciphertext regardless of where the circuit ended.
The niobium-client examples already do this (`server.cpp` in `examples/mult`,
`examples/simple_ops`, `examples/plaintext_add`: "decrypt needs only the last
tower"). Make it the last server operation, after any final bootstrap.

- **Saves:** (L_out + 1) MiB → 1 MiB per output ciphertext.
- **Costs:** none at run time. Note the precondition in the API: `towersLeft`
  must be at least the ciphertext's noise scale degree, so compress after the
  final rescale (FLEXIBLEAUTO handles this; with manual scaling, rescale first).
- **Needs:** OpenFHE ≥ 1.1 (present in the image).
- **Re-validate:** the normal gate. The decode margin at one limb is the Stage 6
  inequality with `depth − levels = 0`; a result that only decoded because it had
  spare limbs will fail here, which is a real finding about the design, not
  about Compress.

### C2. Return less

Covered in Stage 5 and restated: emit a margin instead of a pair of logits, emit
normalized outputs and apply any physical scale at the client, and pack several
outputs into one ciphertext's slots when the packing allows it. Each halves or
better the download for free.

## D. Serialization and transport

- **Binary serialization only.** OpenFHE's `SerType::JSON` is several times the
  size of `SerType::BINARY` for the same object; the four programs should already
  use BINARY, so confirm it rather than assume it.
- **General-purpose compression does not help.** RNS limbs are uniformly
  distributed words; gzip/zstd gain a few percent at best and cost CPU on both
  ends. Measure once, record the number in the ledger, and do not ship it.
- **Ship keys once, address them by fingerprint.** Setup material (context,
  public key, evaluation keys) ships once per key pair; every request carries a
  key fingerprint so the server can refuse a mismatch instead of producing
  garbage. This is the Stage 9 message flow stated as an optimization: a
  protocol that re-sends keys per request is paying A-class costs at B-class
  frequency.
- **Stream batches.** For a multi-batch protocol, send and process slot-batches
  as a stream rather than as one upload, so peak server memory is one batch's
  working set and the first results return before the last batch arrives.

## E. Compute-side optimizations

Server time and memory are governed by circuit structure, which is Stage 5 and
Stage 6 material: packing for the dominant operation, batching to amortize,
rotation frugality (BSGS, hoisting, key-affinity), modest `dnum`, depth
reduction, and the plaintext encode-ahead pipeline. All of it is in
`references/performance-and-packing.md`, in the order to apply it. Stage 11 adds
only the discipline: profile by primitive first (bootstraps, rotations,
relinearizations, Chebyshev evaluations), change one thing, re-validate. A
packing change is a circuit change and re-enters Stage 7 with a new twin.

## The candidate table and the choice

Before anything is applied, the user gets one table with every candidate on
it and decides how the pass will run. Build it from the measured baseline
ledger; the **before** column is measured, the **after** column is an estimate
from the sizing model and is labelled as one until a row has been applied and
re-measured.

One row per candidate that could apply to this application, including the
ones that give nothing here (so the user sees they were considered), with:

| column | what goes in it |
|---|---|
| candidate | a plain-language name, not the technique ("compress the result before sending it back", not "Compress(ct,1)") |
| shrinks | which cost: upload / request, download / request, setup transfer (once), server time / request, server memory |
| before → after | measured before, estimated after, in the user's units (MB, seconds, GB) |
| what it costs | in the same units: added server time per request, a one-time key download, a longer first run, a new build |
| needs | what must already be true: the server already refreshes; the SDK has the feature; the depth budget has N levels of room; a build of about this size |
| recommend | apply / skip / defer, in one phrase, with the reason |

Order the rows cheapest first within each cost they shrink, and put the
binding cost's rows at the top. Keep the table to what the deployment cares
about: a user who said setup bandwidth is fine does not need the setup rows
argued, only listed.

Then ask, naming the actor in each option as the communication rules require:

> Which of these do you want applied?
> (a) You choose: name the rows to apply, and the assistant applies them in
>     the table's order, one at a time.
> (b) The assistant chooses: it applies the set that best reduces the cost you
>     name as the one that matters (or the one the ledger shows is binding),
>     cheapest first, within the depth budget and the hardware envelope, and
>     skips anything that needs a build the SDK does not support.

**The rule for option (b).** "Best cost" needs an objective, so first fix the
cost being minimized: the one the user names, else the one the baseline
ledger shows is binding for the stated deployment (a mobile client makes it
upload per request; a key-rotation policy makes it setup transfer; a shared
accelerator makes it server time). Then:

1. Take every row that reduces that cost and whose "needs" are already true
   (no new build, no unsupported SDK feature, depth and envelope within
   budget). These are applied, cheapest first.
2. Among rows that reduce it but need a build (hybrid FHE, seeded keys without
   an SDK API), apply none silently; present the best one as a separate
   decision with its build cost, since a project is not something the
   assistant signs the user up for.
3. Among rows that reduce it at a cost in another dimension (depleted inputs
   buying upload with server time and key memory), apply only if the cost
   dimension is one the user said does not bind, and say so.
4. Rows that do not reduce the objective are skipped, not applied "because
   they are free"; the user can add them under option (a) later.

Whichever option the user takes, the discipline is unchanged: one row at a
time, behind a switch, re-validated to the Stage 7 tolerance. After each
applied row, re-present the table with the measured after in place of the
estimate, so the next row is chosen on real numbers, and record in the ledger
below which option the user took and which rows were applied.

## The optimization ledger

Record every optimization, applied or rejected, in the results report under a
heading of its own. One row each:

| optimization | what it shrinks | before | after | cost paid | validation | status |
|---|---|---|---|---|---|---|
| C1 Compress result | download / request | 9 MiB | 1 MiB | none | --cpu/--sim/Fog: max err 4.1e-5 (tol 2e-3), 0 flips | applied |
| B1 level-matched inputs | upload / request | 26 MiB × 15 | 9 MiB × 15 | none | same bar | applied |
| B2 depleted inputs | upload / request | 9 MiB × 15 | 2 MiB × 15 | +15 bootstraps/request, +1.2 GiB boot keys at server | twin re-derived with refresh noise; same bar | rejected: server did not already bootstrap, key cost > saving |
| A2 seed-expanded keys | setup transfer | 640 MiB | — | needs seeded serialization | not available in installed SDK | deferred |

Rejected and deferred rows matter as much as applied ones: they are the record
that the trade was considered, and the "needs" column is the shopping list for
the next SDK release.

Update the Stage 9 protocol document for every applied row that changes what
crosses the wire or what the server holds: a seed, a symmetric-key ciphertext
and nonce scheme, bootstrapping keys in the server home, a secret-key-bearing
`encrypt` program. The run README's "expected output" block gets the new sizes.

## Explaining the trades to the user

The communication rules from the top of the skill apply with extra force here,
because every optimization is a trade and the user is the one who owns the
budget. The candidate table above is where the trades are presented; each row
in it is written as what it changes in the user's terms, not as a
technique: "each request currently uploads about 200 MB; this change brings it
to about 6 MB and adds roughly 20 seconds of server time per request" is a
decision a user can make; "transcipher the inputs with Rubato at depth 5" is
not. Lead with the measured before/after, name the cost in the same units
(time, money, a new one-time download, a longer first run), say what has to be
true for it to work (the server already refreshes, the SDK supports it), and
give your recommendation. For a user new to FHE, the newcomer reference's
vocabulary substitutions cover the terms that come up here: "refresh" for
bootstrap, "the one-time key download" for evaluation keys, "the compact
envelope" for a symmetric ciphertext.
