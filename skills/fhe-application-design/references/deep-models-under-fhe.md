# Deep sequential models under FHE (transformers, deep nets)

The worked examples elsewhere in this skill are shallow circuits (a distance, a
small MLP, an autoencoder). Deep sequential models — transformers, deep neural
nets — are a different regime, and this playbook is what we learned building a
full 12-layer transformer (BERT-base) end to end under CKKS. **Read the honest
caveat first: we do not have a reliable general solution to the noise-growth
problem these models pose (§3). Treat depth as a feasibility risk to measure
early, not a solved cost.**

## 1. Why deep models are a different regime

- **Many sequential nonlinearities.** Each attention softmax, each LayerNorm,
  each activation is a polynomial approximation *plus* a bootstrap. A 12-layer
  transformer has ~24 LayerNorms, ~12 softmaxes, ~12 activations, and on the order
  of a thousand-plus bootstraps per forward pass. Bootstrapping is mandatory and
  dominates runtime.
- **A residual stream that accumulates.** Every block adds to the residual
  (`x += attn(x)`, `x += ffn(x)`). Its magnitude grows across depth; LayerNorm
  resets the *local* scale but the accumulated **noise** rides straight through.
- **Wide dense layers.** The matmuls are cheap in *depth* but their additive
  fan-in is a hidden noise cost (~log₂N bits per correlated accumulation — see the
  additive-fan-in note in Stage 6).
- **The binding resource shifts.** For shallow circuits the question is "does it
  fit the depth budget." For deep models it becomes "does the accumulated CKKS
  noise stay under the decode scale by the last layer" — a **noise-budget**
  problem, message-dependent and data-dependent.

## 2. Per-layer anatomy and the hard-won rules

Map each transformer sub-layer to its FHE realization and the rule that keeps it
sound:

- **Linear projections (QKV, output, FFN 768↔3072).** Matmuls: 1 level each,
  cheap in depth. Cost is rotations (key-switching, bandwidth-bound) and fan-in
  noise. *Rule: design rotation-frugal (hoist, BSGS, key-affinity); budget the
  fan-in into noise headroom, not depth.*
- **Attention / softmax = exp then reciprocal (row-normalize).** *Rules:* fit exp
  and 1/x on the sim's **own** operand envelopes (the row-max tree shifts the
  denominator vs the reference); a small attention denominator makes 1/x steep — a
  noise amplifier — so watch the reciprocal domain; the numerators must not
  bootstrap above \|m\|=1 (fold a scale first).
- **LayerNorm = 1/√var.** *Rules:* the **raw variance must never bootstrap** — use
  a coupled Newton (y, z = v·y²) so variance and seed share a segment; boot
  prescales are **per-channel vectors**, never a scalar (one ±outlier channel
  otherwise amplifies noise across all channels); floor the variance with an
  additive `ln_eps` so a tiny-variance token stays in the Newton basin.
- **GELU / activation.** *Rule:* composite it (low-degree compressor → boot →
  high-degree corrector) with a **monotone** stage-1 surrogate (GELU's dip makes a
  naive self-composite 2-to-1), and **clamp the high-degree tail** so a
  noise-drifted operand saturates instead of exploding as `T_deg` (see the
  in-circuit-noise-drift mode in Stage 4).
- **Residual add.** Free in depth, but it is the conveyor that carries the
  accumulating noise to the next block. This is the mechanism behind §3.

## 3. The central problem: noise growth across depth (unsolved in general)

Down the residual stream, CKKS noise accumulates: multiplication noise is
message-dependent (≈ m·e), magnitudes grow with depth, and LayerNorm resets scale
but not noise. By the deepest layers the accumulated noise crosses the decode
scale for a **data-dependent fraction** of inputs — a reject rate. On our
12-layer transformer at N=2¹⁶/128-bit this was ~40%.

Be honest about what we know:

- It is **encryption noise, not polynomial error** (the noiseless twin, carrying
  the same polynomials, produces correct margins for the rejecting inputs).
- It is **not offline-predictable** — a *passing* input can carry *more* measured
  noise at a mid-circuit checkpoint than a *rejecting* one; no aggregate offline
  quantity separates the two sets (see debugging §6).
- **We have no reliable general fix.** What we have is partial, in rough order of
  leverage:
  1. **Magnitude control** — exploit every norm to reset the working scale;
     per-channel folds; isolate/suppress outlier channels. Bounds growth; does not
     eliminate it.
  2. **Precision / margin** — size the scaling modulus for the deep end; buy a
     spare level *if* the security/hardware envelope allows (at a fixed ring it
     often does not — you are at the ceiling).
  3. **Bootstrap placement** — refresh right after magnitude peaks.
  4. **When those aren't enough** — accept a **runtime reject rate** (reject-aware
     decrypt) and, if the rate is too high, **co-design**: bounded-activation /
     outlier-suppressed retraining plus hardware capacity (see the co-design
     escalation in Stage 6). This is the honest production path, not a circuit tweak.

The open question — a general, offline-predictable, low-reject-rate recipe for
deep models under CKKS at fixed hardware — is **unsolved**. Say so to the user
rather than implying depth is a solved cost.

## 4. Design discipline for deep models

- **Estimate the noise budget in Stage 2, early.** Depth × width, bootstrap count,
  additive-fan-in bits → is the design near the security/hardware ceiling? If yes,
  it is noise-marginal and you should expect a reject rate. Surface that before
  building.
- **Give the twin a noise model that predicts the reject rate.** Per-op noise ∝
  operand magnitude plus the fan-in bits, calibrated once to measured hardware
  noise, so you watch the predicted reject rate fall as you tighten the design —
  the cheaply-falsifiable check from the co-design section — before spending
  encrypted runs.
- **Validate on the worst-case data composition**, not a base-rate sample — the
  reject rate is data-dependent, so a data-heavy or class-imbalanced batch is the
  real test.
- **Segment and checkpoint** the execution (for memory *and* debuggability — see
  debugging-the-encrypted-run).

## 5. The two levers when you are stuck

Neither is free, and production usually needs both:

- **Model co-design** — retrain with bounded activations and outlier suppression
  so the residual stream and outlier channels stop driving the noise (Stage 6
  co-design). This is the fixed-hardware lever.
- **Hardware capacity** — a larger-memory accelerator, multi-accelerator spanning,
  or a larger ring (N=2¹⁷) for genuine spare levels. This is the fixed-model lever
  (Stage 0 profiles).

The one thing not to do is treat deep-model noise growth as a parameter-tuning
problem. At the ceiling it is a co-design problem; tuning alone will chase its
own tail.
