# Debugging the encrypted run

The faithful twin validates the *circuit*, not the *crypto runtime*. When the
twin (and the slot-level simulation) pass but the encrypted run fails, the bug
lives in the runtime — noise, magnitude, bootstrap, or hardware — and it needs
its own localization technique. This is that playbook.

Two things make it tractable, and you should already have both from earlier
stages: the **noiseless sim / faithful twin as an expected-value oracle**, and
the tape run as **checkpointed segments** (adopted for memory in Stage 8, reused
here as the debugging substrate). If you are not segmenting yet, add
live-register checkpointing at op boundaries before you start — bisection depends
on it.

## 1. Triage: which failure is this?

Three shapes, three sub-playbooks — don't conflate them, because they look
similar and need opposite fixes:

- **A crash** (server abort, out-of-memory, a segment that never returns). This
  is a *resource/implementation* failure, not a numeric one: device memory
  exhausted (evaluation keys + working set), the allocator's high-water creep, a
  missing rotation key, a serialization mismatch. Fix the resource — it is not the
  numeric path below. (Our own example: `dnum`-6 key-switching keys that overran
  a 32 GB card mid-segment — an OOM, not a decode error.)
- **An undecodable result** — `Decode` throws *"the approximation error is too
  high."* This is numeric, and it has three distinct causes (§5): a magnitude
  escape, a bootstrap-magnitude violation, or noise-budget exhaustion.
- **A decodable but wrong result** — it decrypts fine but the margin/decision
  disagrees with the twin. This is *not* a runtime bug at all: it is polynomial
  approximation error or a calibration/bounds mistake. Compare margins to the
  twin, check the poly domains and the calibrated bounds; the fix is design-side.
  Remember `Decode` is a check on CKKS *decryption noise* vs the scale —
  polynomial error yields a wrong-but-decodable value, never an undecodable one.

## 2. First move: deterministic, or transient?

Before localizing anything, **rerun the failing input once.** Non-ECC
accelerators running multi-minute-to-hour circuits produce silent transient
corruption; a transient passes on retry, a real bug fails *identically* — same
symptom, same location. Don't spend a bisection on a coin flip. Only once it
repeats deterministically do you localize.

## 3. Localize: checkpoint bisection

Turn "somewhere in N million ops" into "op ≈ k" in a handful of runs:

1. Decrypt **one representative live register per segment boundary.** The first
   boundary whose registers will not decode brackets the bad segment — everything
   before it is clean.
2. **Fine-bisect inside that segment:** resume from the last good checkpoint and
   re-run `[seg_start, mid)` with intermediate checkpoints, decrypt, and narrow.
   Because you resume from a checkpoint rather than op 0, each probe is cheap.
3. Stop when you have a narrow op range — tens of ops — around the first
   undecodable register.

## 4. Classify: decrypt-and-compare against the sim

The noiseless sim is your oracle for what each register *should* hold.

- Decrypt the device's intermediate registers and compare to the sim's values at
  the **same** registers. `device − noiseless-sim` **is the accumulated CKKS
  noise** — the polynomial-approximation error is identical on both sides and
  cancels, leaving only encryption noise.
- **A sudden jump** in the deviation localizes a noise *source* (a specific op);
  **a smooth ramp** that crosses the scale says budget *exhaustion* (§5).
- **Compare a failing input against a passing one** at the same checkpoints. If
  the failing input is indistinguishable offline — operand magnitudes, bootstrap
  magnitudes, Chebyshev domain-margins, and polynomial steepness all match the
  passing input — then the discriminator is the noise *realization*, not the
  circuit (§6).
- To line up device registers with sim values, map through the finalize
  renumbering by **parsing the on-disk tape** (ops + plaintext index), not by
  rebuilding the whole tape in memory. **Capture only the target registers** —
  retaining every intermediate value is exactly how you OOM the *design host*
  while chasing a device bug.

## 5. The three numeric failure modes, and their fixes

| mode | offline signature | fix |
|---|---|---|
| **Magnitude / domain escape** — an operand drifts past a fitted Chebyshev domain; `T_deg` extrapolates as ~cosh(deg·√2ε) and overflows the scale (catastrophic at high degree) | the sim's cheb domain-margin at that site is razor-thin, or an operand magnitude there exceeds the fit range | widen the domain to the measured *in-compute* range (not the noiseless one); **clamp the high-degree tails** where extrapolation is catastrophic (deg ≳ 50) and exact-fit below; or fold a prescale. See the *in-circuit noise drift* failure mode in Stage 4. |
| **Bootstrap-magnitude violation** — a bootstrap operand \|m\| > 1, so EvalMod's sine approximation diverges | the always-on boot-magnitude guard trips | fold a scale *down* before the boot; multiplicative folds must **track value ratios** (a large compensating fold injects noise). |
| **Noise-budget exhaustion** — accumulated CKKS noise exceeds the decode scale, with no single escape | gradual deviation growth; data-dependent; **not** offline-predictable | buy a spare level if the security/hardware envelope allows; else model/hardware co-design; else accept a **runtime reject rate**. See the co-design escalation in Stage 6. |

## 6. When the sim cannot reproduce it

Sometimes the failure is deterministic on the device yet invisible offline: the
sim — even with injected per-refresh noise — cannot distinguish the failing
inputs from the passing ones (a *passing* input can even carry *more* measured
noise at a mid-circuit checkpoint than a failing one). When that happens, the
discriminator is the CKKS noise realization in the deep layers, which no
aggregate offline quantity captures. Two consequences follow, and both save you
from a tail-chase:

1. **No client-side reject predictor is buildable from the sim** — stop trying to
   build one; the signal isn't there.
2. **It is a reject-rate phenomenon** → handle it with **runtime rejection**
   (reject-aware decrypt), and if the rate is too high for the application,
   escalate to the co-design route (Stage 6), not to more parameter tuning.

## 7. Turn runtime failures into build failures (prevention)

Every guard you can evaluate on the sim is a device failure you never have to
bisect:

- **Degree-aware Chebyshev domain-escape guard** — fail the *build* when an
  operand would escape a fit domain, so a domain bug never reaches the device.
- **Always-on boot-magnitude guard** — assert \|m\| ≤ 1 at every real bootstrap,
  including in noiseless calibration (a noiseless build otherwise never checks it).
- **Value-ratio fold check** — flag multiplicative plaintext folds whose
  max/min-nonzero ratio would amplify noise.
- **Reject-aware decrypt** — catch the `Decode` exception and mark the inference
  rejected rather than aborting the whole run.

## 8. Operational discipline while debugging

- **Single-thread the sim** (`OMP_NUM_THREADS=1` etc.) — calibration/sim is often
  much faster single-threaded on a design host and stays deterministic.
- **Don't retain what you don't need** — when you only need bounds, don't keep the
  full op list or plaintext store (a `record=false` mode); scope value captures to
  the registers under comparison.
- **Mind the design host's own memory** — the instrumentation that captures
  intermediates is itself a memory hazard; a naive "save every register" is a
  self-inflicted OOM, separate from anything happening on the accelerator.
