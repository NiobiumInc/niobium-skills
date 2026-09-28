# Performance: packing, amortization, and a key-switch-frugal circuit

Performance in FHE is not a post-hoc optimization pass — it is decided by a few
structural choices made during circuit design (Stage 5) and parameter selection
(Stage 6). This reference centralizes them. The one-sentence version: **pack for
throughput, and design so the circuit does as little key-switching as possible**,
because SIMD slots are the unit of useful work and key-switching is the dominant
cost.

## 1. Mental model: slots are the unit of throughput

A CKKS ciphertext has N/2 SIMD slots (32,768 at N=2¹⁶). Every homomorphic
operation acts on all slots at once, at the same cost. So the first performance
question is not "how fast is an add" but **"how much useful work rides each
ciphertext?"** Packing — how you lay data into slots — determines both how many
results you get per op and how many **rotations** the circuit needs (rotation is
the expensive primitive; see §4). Packing is a *circuit-design* decision, not a
tuning knob.

## 2. Packing / data layout

Choose the layout that makes the **dominant operation** cheap:

- **Batch packing (amortize the whole circuit).** Put many independent items in
  the slots — e.g. B records, or B sentences of a transformer batch — and the
  entire circuit runs once for all B. This is throughput ×B for free; it is the
  single biggest performance lever for inference workloads.
- **Feature-major vs record-major.** Whether features or records occupy
  contiguous slots decides which reductions are free (aligned adds) and which need
  rotations. Pick the axis so the matmul/attention you do most is rotation-light.
- **Replication / broadcast.** Matmuls under FHE are diagonals + rotations
  (baby-step/giant-step) with slot replication for broadcasting a vector across a
  tile. Design the layout so these patterns line up with power-of-two rotations
  you already hold keys for.

The worked examples show concrete layouts (column-major set-membership,
feature-major intrusion detection); reuse the one whose dominant op matches yours.

## 3. Amortization: throughput vs latency

- **Latency** — one inference's wall time — is largely fixed by depth × per-op
  cost + bootstraps, and is high and mostly irreducible for a deep circuit.
- **Throughput** — results/second — is what you actually optimize: SIMD batching
  packs B results per ciphertext, and the work is **embarrassingly parallel**
  across ciphertexts, worker processes, and Fog accelerators.

For almost every FHE application the right framing is throughput, not latency:
batch wide, run many in parallel. Report both, but design for throughput. (State
this explicitly to users who expect low single-query latency — FHE does not give
that; it gives batched throughput.)

## 4. The key-switch-frugal circuit (the performance core)

Key-switching — every rotation and every relinearization — is the dominant cost
and is bandwidth/capacity-bound (it streams large evaluation keys; see the Stage-0
hardware envelope). So the core of FHE performance is doing **less** of it:

- **Baby-step/giant-step (BSGS)** matmuls — O(√n) rotations instead of O(n).
- **Hoisting** — decompose the input **once** and reuse that decomposition across
  all rotations that share the same source ciphertext (the expensive ModUp is
  amortized). BSGS attention/matmul is the natural place.
- **Key-affinity scheduling** — order the program so uses of the same rotation key
  are adjacent, and each key streams from memory once.
- **Fewer distinct rotation keys** — each key is a large object to store and
  stream; a layout that needs 60 rotations is cheaper to run and provision than
  one that needs 600.
- **Modest `dnum`** — a larger `dnum` shrinks the special modulus but *enlarges*
  the key-switching keys and adds decomposition NTTs; key memory is the wall.
- **Rotate-and-sum** (log N) for slot reductions — free in depth, but it consumes
  rotation keys, so fold the count into the plan.

## 5. The plaintext / weight pipeline

Weight-heavy models (transformers, large MLPs) multiply by hundreds of thousands
of plaintext diagonals. Two facts drive the design:

- **Do not pre-encode everything.** Fully-expanded encoded weights can reach
  terabytes; store the **compact** form (e.g. a per-feature base + roll) and
  expand on demand.
- **Overlap encoding with compute (encode-ahead).** A producer encodes plaintexts
  just ahead of the consumer (the accelerator), so encode time hides under compute
  instead of serializing in front of it. A cold, serial encode path can dominate
  wall time (hours); a warm, overlapped one brings it back to the compute floor.

## 6. Depth is a performance resource, not just a correctness one

Depth drives parameter size, which drives memory and per-op cost. Reducing depth —
tree-structured reductions, lower-degree approximations where precision permits,
deferred relinearization, boundary-representation changes — is a *performance*
lever as much as a feasibility one. Bootstrapping trades depth for refresh cost;
budget both together (Stage 6).

## 7. Measure the right things

Profile by primitive, not by wall clock alone: **bootstraps** (typically the
largest single share of time), **rotations** (key-switching), ciphertext×ciphertext
**multiplies** (relinearization), and Chebyshev evals. Size against current
benchmarks for your target, not against peak hardware specs (the peak vs effective
gap is real; see Stage 0). Report throughput (results/second at your batch width)
alongside single-inference latency.

## 8. The order to apply these

1. **Pack** for the dominant operation (layout).
2. **Batch** to amortize the circuit across items (throughput).
3. **Minimize key-switching** — BSGS, hoisting, key-affinity, few keys, modest
   `dnum`.
4. **Overlap** the plaintext/weight encode with compute.
5. **Reduce depth** where precision allows.
6. **Parallelize** across workers and Fog accelerators.

Do these in order: a rotation-frugal layout beats micro-optimizing an op, and
throughput batching beats shaving latency.
