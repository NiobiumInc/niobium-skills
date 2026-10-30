# Approximating non-linear functions (Stages 3, 5, 6, 7)

Every sigmoid, tanh, GELU, comparison, division or square root in a design
becomes a polynomial under CKKS, and the gap between that polynomial and the
function is usually the largest error in the whole application, larger than
encryption noise by orders of magnitude. SKILL.md's Stage 3 feasibility check
decides *whether* a polynomial can be made safe and accurate inside the depth
budget. This reference is about *how*: which errors there are, how to fit, what
to fit, what to measure, and when to stop fitting and change the model instead.

Contents:
- [Three errors, three fixes](#three-errors-three-fixes)
- [Degree and depth: what a series really costs](#degree-and-depth)
- [The coefficient-norm rule](#the-coefficient-norm-rule)
- [What to approximate](#what-to-approximate)
- [How to fit: four methods and when each wins](#how-to-fit)
- [Non-smooth targets: compose, don't fit](#non-smooth-targets)
- [Reciprocals and roots: iterate, don't fit](#reciprocals-and-roots)
- [Downstream amplification](#downstream-amplification)
- [Calibration and polynomial-aware fine-tuning](#calibration-and-fine-tuning)
- [The workflow, Stage 3 to Stage 7](#the-workflow)
- [Sources](#sources)

## Three errors, three fixes

A measured end-to-end error is the sum of three things that need opposite
treatment. Separate them before touching anything.

| error | what it is | visible in the twin? | fix |
|---|---|---|---|
| **approximation bias** | the polynomial is not the function: `p(x) − f(x)` | yes, exactly | a better fit, a better target, a tighter domain (this reference) |
| **evaluation noise** | CKKS arithmetic noise through the series evaluation: `Dec(p(Enc x)) − p(x)` | no | a larger scaling modulus, or a fit with a smaller coefficient 1-norm; **not** more degree |
| **downstream amplification** | whatever follows the approximation multiplies both errors by its derivative chain | yes for bias, no for noise | budget the error where it is amplified least; re-center; see below |

`fhe-cheb-lab`, shipped in the FHE-dev image (`environment/cheb_lab.cpp`),
reports the first two as separate numbers for any series at any parameters,
in seconds: it evaluates the series under CKKS on a dense grid (and on your
own operand samples) and prints `bias` (polynomial vs. function, in double),
`noise` (encrypted vs. plaintext polynomial) and `total`, plus where the noise
peaks and a verdict line. Measured on OpenFHE 1.5.1 (toy ring 2^14, scaling 45,
OpenFHE's own interpolation coefficients):

| series | bias | noise | which dominates |
|---|---|---|---|
| sigmoid on [−8, 8], degree 27 | 1.4e−5 (2^−16) | 3.4e−9 (2^−28) | the fit |
| sigmoid on [−8, 8], degree 59 | 6.6e−11 (2^−34) | 4.2e−9 (2^−28) | encryption |
| tanh on [−8, 8], degree 27 | 5.4e−3 (2^−7.5) | 6.7e−9 | the fit, badly |
| ReLU on [−1, 1], degree 31 | 1.6e−2 (2^−6) | 9.8e−9 | the fit, and no degree fixes it |

The scaling sweep is the signature that tells the two apart without any
theory: sigmoid degree 59 at scaling 35, 40, 45, 50, 55 gave noise of 2^−17.3,
2^−22.6, 2^−27.8, 2^−32.8, 2^−37.3 — **one bit per scaling bit** — while bias
sat at 2^−33.8 throughout. If a number falls with scaling it is noise; if it
does not, it is the fit.

Two more facts the lab measured that matter for reading its output:

- **Noise grows with the ring.** The same sigmoid series at scaling 45 gave
  2.1e−9 at N = 2^13, 4.7e−9 at 2^14, 1.2e−8 at 2^15 and 2.6e−8 at 2^16
  (confirmed at 2^16 / 128-bit in the Niobium image: 3.6e−8). A toy-ring
  measurement understates production noise by **about 4 bits**; run the lab at
  the production ring before trusting a margin.
- **The verdict depends on the degree.** At degree 27 the sigmoid fit dominates
  by 12 bits; at degree 59 encryption dominates by 6 bits. Past the crossover,
  more degree buys nothing and costs a level.

## Degree and depth

OpenFHE evaluates a degree-*d* Chebyshev series with Paterson–Stockmeyer, and
the skill's rule of thumb charges `⌈log₂(d+1)⌉ + 1` levels. The lab measures the
levels actually consumed (counting a rescale FLEXIBLEAUTO leaves pending), and
the cost does not follow that formula: it **steps up right after each degree
of OpenFHE's Paterson–Stockmeyer table**, and every degree above an entry pays
the next level.

| degree | ≤ 5 | 6–13 | 14–27 | 28–59 | 60–119 | 120–247 | 248– |
|---|---|---|---|---|---|---|---|
| measured levels (OpenFHE 1.5.1, FLEXIBLEAUTO) | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
| rule `⌈log₂(d+1)⌉+1` at the top of the band | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
| rule at the bottom of the band | 3 | 4 | 5 | 6 | 7 | 8 | 9 |

So the rule is right only at the table degrees **5, 13, 27, 59, 119, 247** and
one level short just above them: 15 costs 6 levels, not 5; 28 to 63 all cost
7; 127 costs 9. The cheapest degree at each depth is the table entry, which
the skill's examples already use. `cheb_fit.py` charges depth from this table
(and `--depth D` picks the table degree), and when a design is one level from
its ceiling, measure the degree with the lab rather than trusting a formula.

## The coefficient-norm rule

Evaluation noise is not a property of the parameters alone; it is proportional
to the series' **coefficient 1-norm**, Σ|cₖ| in the Chebyshev basis (each
|Tₖ| ≤ 1, so the norm bounds the output and the noise alike). Measured at
degree 27, scaling 45, ring 2^14, OpenFHE's interpolation coefficients:

| function and domain | Σ\|cₖ\| | evaluation noise | noise ÷ norm |
|---|---|---|---|
| sigmoid [−8, 8] | 1.9 | 3.4e−9 | 1.8e−9 |
| tanh [−8, 8] | 2.3 | 6.7e−9 | 2.9e−9 |
| GELU [−6, 6] | 8.7 | 7.7e−9 | 0.9e−9 |
| exp [−1, 1] | 4.0 | 1.6e−8 | 4.0e−9 |
| exp [−4, 4] | 66 | 3.0e−7 | 4.6e−9 |
| exp [−8, 8] | 3,410 | 1.4e−5 | 4.2e−9 |
| exp [−8, 8] with the output divided by e⁸ | 1.1 | 5.6e−9 | 4.9e−9 |

The last column is constant to within a factor of a few: **noise ≈ (a floor set
by scaling and ring) × Σ|cₖ|**. Three consequences:

- The norm is a property of the **function on the domain**, not of the fitting
  method: at the same degree, interpolation, least squares, weighted and
  minimax fits agree on Σ|cₖ| to three or four digits (sigmoid 1.418, GELU
  6.765, 1/x on [0.1, 1] 10.00, in every method). `cheb_fit.py` prints it so you
  can see this; choose the method on error, and spend the norm budget on the
  domain and the output scale.
- A function with a large output range pays in absolute noise exactly in
  proportion. **Scale the output down inside the coefficients** (the exp row
  above) and apply the scale in cleartext or in the next plaintext operand;
  this is the Stage 5 "emit normalized outputs" rule applied to every
  approximation, and it is free.
- A norm that is enormous (exp over a wide domain gives Σ|cₖ| ≈ 2^12 at
  [−8, 8] and 2^39 at degree 45) is a sign the domain is wrong for the
  function, not a parameter problem.

## What to approximate

The degree you can afford is fixed by depth, so the lever is the target.

- **Smooth, bounded targets converge geometrically.** For a function analytic
  on the fit interval, Chebyshev error falls like ρ^−d for some ρ > 1 set by
  how far the nearest singularity is from the interval (sigmoid's poles sit at
  ±iπ, so the wider the real domain, the smaller ρ). Degree buys bits.
- **Non-smooth targets converge algebraically.** ReLU, |x|, sign, step and max
  have a kink or a jump; a single polynomial's error falls like 1/d (and a
  degree-d fit cannot resolve a transition narrower than about 1/d, which is
  why the encrypted-search design measured 109 false positives at degree 512
  when its decision margin matched the transition width). Degree buys almost
  nothing; see "compose, don't fit".
- **Tighten the domain before raising the degree.** Halving the fit interval
  of an analytic function is worth several degrees. The Stage 3 ladder
  (measured band, clipping, filtering) exists for this reason, and per-unit
  domains (Stage 5) make it free under column-major packing.
- **Use symmetry.** An odd target (tanh, sign, x·σ(x) − x/2, erf) needs only
  odd terms: half the coefficients, a smaller norm, and no even-term leakage.
  `cheb_fit.py --auto-symmetry` detects and uses it.
- **Approximate the composition.** Fit GELU directly rather than erf and
  multiply; fit the logit's final sigmoid together with any affine scale that
  precedes it; fit softmax's exponent with range reduction folded in. Every
  intermediate approximation adds its own bias, norm and levels.
- **Move the hard part out.** A client-side transform (Stage 5's boundary
  representation) can turn a comparison into a distance, a normalization into
  a plaintext scale, or a hash into an exact match, and remove the polynomial
  entirely.

## How to fit

Four methods, all available in `scripts/cheb_fit.py`, all emitting coefficients
in OpenFHE's `EvalChebyshevSeries` convention (`c₀/2 + Σ cₖTₖ` on [a, b]) so the
twin and the circuit evaluate the same series.

| method | what it minimizes | when it wins | watch for |
|---|---|---|---|
| **interpolation** (`interp`) | nothing; samples f at the Chebyshev nodes (this is OpenFHE's `EvalChebyshevCoefficients`) | smooth targets at generous degree; it is near-best for analytic functions | wastes accuracy where no operand lands; the only one OpenFHE computes for you |
| **least squares** (`ls`) | mean-square error over the interval | smooth targets when RMS matters more than the worst case | can overshoot at the edges |
| **data-weighted least squares** (`wls`) | mean-square error under the measured operand density | ML activations: operands are concentrated, tails are rare, the task metric is an average | needs the Stage 3 samples; must be paired with a tail policy (clip or filter), since it is bad where it did not train |
| **minimax** (`minimax`) | the worst-case error over the interval (discrete Remez via Lawson's iteratively reweighted least squares) | comparisons, sign, anything where one escaped operand breaks a decision; the composite building blocks | equioscillation check in the tool's output; larger norm than `ls` sometimes |

The decision is data: run the tool on the measured operands and read the
sample-max, the p99.9 and the mean side by side. Measured with the tool
(OpenFHE's convention, same degree and depth in every row):

| target | metric | interp (OpenFHE default) | ls | wls | minimax |
|---|---|---|---|---|---|
| sigmoid [−8, 8], degree 27, 20k samples ~ N(0, 2.5²) clipped | sample max | 1.39e−5 | 1.28e−5 | 5.00e−5 | **8.84e−6** |
| | sample p99.9 | 1.38e−5 | 1.28e−5 | 4.29e−5 | **8.84e−6** |
| | sample mean | 6.17e−6 | 6.86e−6 | **4.16e−6** | 5.60e−6 |
| sigmoid [−8, 8], degree 59, same samples | sample max | 6.56e−11 | 6.10e−11 | 3.77e−10 | **4.17e−11** |
| tanh [−4, 4], degree 15 | interval max | 2.77e−3 | 2.52e−3 | (= ls) | **1.76e−3** |
| GELU [−6, 6], degree 31 | interval max | 7.33e−7 | 5.89e−7 | (= ls) | **4.74e−7** |
| ReLU [−1, 1], degree 27 (depth 6) | interval max | 1.78e−2 | 1.17e−2 | (= ls) | **5.40e−3** |
| sign [−1, 1] \ |x|<0.02, degree 119 (depth 8) | max outside the gap | 2.82e−1 (Gibbs) | 1.19e−1 | (= ls) | **4.24e−2** |
| 1/x [0.1, 1], degree 15 | interval max | 5.63e−4 | 3.70e−4 | (= ls) | **2.44e−4** |

What the numbers say:

- **Minimax wins the max and the p99.9 everywhere**, by 1.2 to 1.6× over
  interpolation on smooth targets, 3.3× on ReLU and 8× on sign, where
  interpolation's Gibbs overshoot (0.28) is the whole story.
- **Least squares is a free improvement over OpenFHE's default** on every
  target, same degree, same depth, same norm. If you change nothing else,
  change this.
- **Weighted least squares buys the mean and pays for it in the tail**: at
  degree 27 it has the lowest sample mean (1.5× better than minimax) and a 4
  to 6× worse sample max and p99.9, because the fit puts almost no weight on
  |x| > 7 where 0.3% of the samples live. Use it only when the task metric is
  an average over operands and the tail is clipped or filtered anyway; for a
  decision at a threshold, the tail is the metric and minimax wins.
- Past the crossover with encryption noise (sigmoid at degree 59, errors of
  1e−11 against a noise floor of 1e−9 at scaling 45) the method stops
  mattering; spend the level elsewhere.

A multi-interval minimax fit (minimax over a union of intervals, excluding a
gap) is the right tool for sign-like targets with a dead zone and for the
bootstrapping sine (Lee, Lee, Lee, Kim and No, ePrint 2020/552); the tool's
`sign` and `step` fits exclude |x| < ε this way.

Two tools, one contract (the fit tool needs only numpy; on a machine without
it, run it in the FHE-dev image: `docker run --rm -v "$PWD":/w -w /w
ghcr.io/niobiuminc/fhe-dev:latest python3 -I <skill>/scripts/cheb_fit.py …`).
`cheb_fit.py --emit METHOD --json fit.json --twin twin.py` writes the coefficients for the circuit (one number per line for the
C++ side, or the JSON) and a numpy evaluator for the twin; `fhe-cheb-lab
coeffs:fit.txt:sigmoid a b d` then measures that exact series under
encryption. The twin never evaluates the true function in place of the
polynomial (the Stage 3 trap) because it evaluates the emitted series.

## Non-smooth targets

For sign, step, comparison, ReLU, max and absolute value, stop fitting one
polynomial. The method is **composition**: a low-degree odd polynomial that
pushes values away from zero toward ±1, applied k times, reaches a precision
that a single polynomial of the same total depth cannot approach.

- Cheon, Kim and Kim (ePrint 2019/1234, Asiacrypt 2020) introduced the
  composite approach to the sign function with optimal asymptotic complexity:
  f(x) = (35x − 35x³ + 21x⁵ − 5x⁷)/16 iterated, each application costing the
  depth of a degree-7 series.
- Lee, Lee, No and Kim (ePrint 2020/834; IEEE TDSC 2022) replaced the fixed
  polynomial with **minimax** components found by a modified Remez algorithm
  and showed the composition is optimal in depth and in the number of
  non-scalar multiplications for a target precision.
- The skill's "squared distance + iterated squaring" set-membership circuit is
  the crudest member of this family (x ↦ x² pushes the match toward 1 and the
  non-match toward 0); the composite polynomials above do the same job with
  far fewer levels per bit of separation.

`cheb_fit.py` prints the composite rows next to the single-polynomial minimax
at the same total depth for `sign`, `step` and `relu` (ReLU as x·(1 + sign(x))/2,
one extra level), with the error measured outside a dead zone |x| < ε that you
set from the decision margin. The measurement is more nuanced than the
folklore "composition always wins", and worth having in front of you:

| sign on [−1, 1], ε = 0.02 | depth | max error outside the gap |
|---|---|---|
| single minimax, degree 13 | 5 | 0.66 |
| CKK f composed once | 5 | 0.96 |
| single minimax, degree 119 | 8 | 0.042 |
| CKK f composed twice | 10 | 0.90 |
| CKK f composed 5 times | 25 | 0.21 |
| CKK f composed 6 times | 30 | 6.6e−3 |
| CKK f composed 7 times | 35 | 8.2e−9 |
| CKK f composed 8 times | 40 | 2.2e−16 |

The fixed polynomial f has slope 35/16 ≈ 2.2 at zero, so lifting a gap of
0.02 to order one takes about five stages before the sharpening starts, and
at low depth a single minimax polynomial is simply better. **Composition is a
depth-for-precision trade, not a depth saver, at these gaps**: it is how you
reach a true step (8e−9 at depth 35, roundoff at 40) that no single
polynomial reaches at any practical degree, and it is pointless for a loose
threshold. Two levers move the crossover: a larger gap (the composition's
error is governed by ε, not by degree, so a decision margin of 0.1 instead of
0.02 saves stages), and better components. The tool implements only the fixed
Cheon–Kim–Kim f; Lee, Lee, No and Kim's minimax components (ePrint 2020/834)
are designed to reach a target precision in fewer levels, and are what to
implement when the depth matters. Per stage the tool charges the measured cost
of a degree-7 series (5 levels); a chain may share a pending rescale and come
out a level cheaper, so measure a built chain with the lab.

## Reciprocals and roots

1/x, 1/√x and division are the other place a single polynomial is the wrong
tool: the function is unbounded near zero, so any interval that includes small
values forces a huge norm and a poor fit.

- **Newton iteration** from a cheap initial guess: for 1/x, yₙ₊₁ = yₙ(2 − x·yₙ)
  costs two levels per step and doubles the correct bits each step; for 1/√x,
  yₙ₊₁ = yₙ(3 − x·yₙ²)/2 costs three. A degree-3 or degree-5 polynomial over the
  measured range gives the initial guess; three or four steps reach CKKS
  precision. `references/deep-models-under-fhe.md` uses this for LayerNorm's
  rsqrt.
- **Goldschmidt** division is the same arithmetic arranged so numerator and
  denominator are refined together; it pipelines better when both are
  ciphertexts.
- **Range-reduce first.** Scale the input by a known plaintext factor into
  [0.5, 1] (or [1, 2]) where the initial guess is a short polynomial and the
  iteration converges in few steps, then undo the scale on the output.
- In either case the Stage 6 budget must count every step's levels, and the
  twin must run the same iteration count: the error after a fixed number of
  Newton steps is deterministic and is part of the bias.

## Downstream amplification

An approximation's error does not stay where it is made. The error after the
rest of the circuit is the error times the circuit's derivative along the path,
so the same bias costs very different amounts in different places:

- Nine squarings after a score (the encrypted-search sharpen) amplify an error
  near the threshold by the derivative of x^512, which is why a transition
  width that matched the decision margin produced false positives at degree
  512.
- A sigmoid at the output of a classifier amplifies nothing (its derivative is
  at most 1/4), so bias there is cheap; the same sigmoid inside a hidden layer
  is multiplied by every downstream weight.
- A Newton iteration *reduces* the error of its input, so bias in the initial
  guess is nearly free.

Budget accordingly: spend degree where the derivative chain is long, accept a
cheap fit where it is short, and when a decision sits at a threshold, measure
the bias **in the task metric** (decision flips, recall at the operating point),
never in max polynomial error. This is also why the Stage 6 decode-safety
margin must be checked with the *polynomial* twin: the worst operand for
decode is often one the polynomial, not the function, produces at the domain
edge.

## Calibration and fine-tuning

Two zero-depth corrections remove most systematic error once the polynomial is
chosen, and both belong in the twin before any encrypted build:

- **Recalibrate the decision rule on the polynomial model.** The polynomial
  shifts and squashes the score distribution; a threshold tuned on the true
  model is no longer the operating point. Re-fit the threshold (and any output
  affine scale) on the twin's outputs over the validation set. This usually
  recovers most of a dropped task metric at no cost, and it is the first thing
  to try when a fit "loses accuracy".
- **Polynomial-aware fine-tuning.** Where retraining is allowed, fine-tune the
  downstream layers (or the whole network) with the polynomial activation in
  place, so the weights adapt to the approximation instead of the other way
  round. The literature agrees this closes most of the gap at low degree: a
  per-layer fine-tuning of polynomial activations in place of swapping one
  polynomial into every ReLU (arXiv 2111.03362); handling the activation's
  input range during fine-tuning so low-degree polynomials keep the original
  accuracy, since the input range dominates the error (arXiv 2304.14836); and
  a single-stage fine-tuning that converts pre-trained CNNs to degree-4
  polynomial activations at comparable accuracy (Peregrine, arXiv 2511.18976).
  This is the cheap end of the co-design ladder in SKILL.md Stage 6: it
  targets approximation bias, not the message-dependent noise that full
  quantization-aware training addresses, so it is where to start.

Both are measured the same way: the task metric of the twin against the
reference, before and after. Neither changes the depth.

## The workflow

1. **Stage 3, measure.** For each non-linearity, record the operand
   distribution on the test set: min, max, the p0.1–p99.9 band, and the
   samples themselves (one file per call site, or per unit under column-major
   packing).
2. **Stage 3, budget.** From the depth budget and the number of approximations
   on the critical path, the maximum degree per call; take it from the measured
   table above (5, 13, 27, 59, 119, 247), not from the rule, and never step
   above an entry "for free".
3. **Stage 3, fit and compare.** `cheb_fit.py` on the measured samples with the
   band as the domain: interpolation, least squares, weighted, minimax; read
   sample-max, p99.9, norm. Pick the cheapest method that clears the floor in
   the task metric when the forward pass runs with the real polynomial (never
   the clean twin). For sign-like or reciprocal targets, use the composite or
   Newton rows instead of a single polynomial.
4. **Stage 3, tails.** Decide the tail policy the fit implies: clip, filter, or
   a clamped-tail fit (SKILL.md's in-circuit-noise-drift mode), and run the
   composition guard if filtering.
5. **Stage 5, place it.** Per-unit domains and coefficients where packing makes
   them free; output scale folded into the coefficients; symmetry used.
6. **Stage 6, measure the noise.** `fhe-cheb-lab coeffs:FILE:func` at the
   planned scaling and ring: bias, noise, the scaling sweep. If noise dominates,
   raise scaling or pick the lower-norm method; if bias dominates, go back to
   step 3; if total clears the Stage 7 tolerance with margin, proceed.
7. **Stage 7, the twin.** The twin evaluates the emitted series (`--twin`), the
   same iteration counts, the recalibrated decision rule. Validate on the task
   metric. Record the bias as part of the twin-vs-reference row of the results
   report, separately from the encryption row.

## Sources

- Trefethen, *Approximation Theory and Approximation Practice* (SIAM, 2013): the
  convergence rates behind "smooth converges geometrically, kinks converge
  algebraically", and why interpolation at Chebyshev nodes is near-best.
- Paterson and Stockmeyer, "On the number of nonscalar multiplications
  necessary to evaluate polynomials" (SIAM J. Comput., 1973): the evaluation
  scheme OpenFHE's `EvalChebyshevSeriesPS` uses.
- Cheon, Kim and Kim, "Efficient homomorphic comparison methods with optimal
  complexity", ePrint 2019/1234 (Asiacrypt 2020): composite polynomials for sign.
- Lee, Lee, No and Kim, "Minimax approximation of sign function by composite
  polynomial for homomorphic comparison", ePrint 2020/834 (IEEE TDSC 2022).
- Lee, Lee, Lee, Kim and No, "High-precision bootstrapping of RNS-CKKS
  homomorphic encryption using optimal minimax polynomial approximation and
  inverse sine function", ePrint 2020/552 (Eurocrypt 2021): the improved
  multi-interval Remez algorithm.
- arXiv 2111.03362, "A methodology for training homomorphic-encryption friendly
  neural networks"; arXiv 2304.14836, "Training large scale polynomial CNNs for
  E2E inference over homomorphic encryption"; arXiv 2511.18976, "Peregrine:
  one-shot fine-tuning for FHE inference of general deep CNNs": polynomial-aware
  training and fine-tuning.
- OpenFHE `src/core/lib/math/chebyshev.cpp` (`EvalChebyshevCoefficients`): the
  interpolation and the coefficient convention the tools reproduce.
