# CKTEST-RH-001: campaign analysis

**Campaign:** CKey reader hardening, profiles P0 (Format 0.27, standard) and P0.1 (Format 0.28, print).
**Reader under test:** CKey core `ede38f5` (0.1.0-build.85), via `CKeyDecoder.swift` adapter; harness `0217e28`.
**Gates locked:** 2026-11-08 05:57 UTC by Caleb (`configs/gates.yaml` sha256 `a53d0dae…`).
**Server:** Hetzner `ubuntu-32gb-hel1-1`, 8 workers; released 2026-11-10 by the owner after R2 verification.
**Evidence:** `evidence/` (this branch, commit `2000b41`); all sealed data in R2 bucket `ckey-hardening-test`.

> Simulated, host-side evidence about the production CKey core. Not a certification.

## 1. Bottom line

1. **Safety holds.** Across 1,364,000 completed cases there is **no accepted wrong payload**: 1,000,000 structured, 350,000 adaptive failure-targeted (the hardest conditions the 1M run found), 4,000 adaptive cross-condition and 10,000 pilot. That includes 100,000 deliberate hard negatives (forged CRCs, t+1 corruption, random codewords, splices, mirrors, foreign grids, blanks). The one-sided 95% upper bound on the wrong-payload rate is **3.0 × 10⁻⁶** from the structured run alone (6.0 × 10⁻⁶ per profile). An independent Reed–Solomon oracle agreed with the production decoder on all 1,000,000 cases.
2. **Read rate is the problem, not trust.** The reader decodes 50.1% of all valid symbols and **68.9% of the physically recoverable ones**. The 31-point gap on recoverable frames is reader headroom: the pixels contain the code but the reader does not get it out.
3. **The campaign did not finish.** The adaptive cross-condition pool stalled after 4 of its 50 shards (16:28 UTC, 9 Nov) and the boundary-search pool and the 100,000-case holdout never ran. The server sat idle for about 10.5 h before it was released.
4. **Label: UNQUALIFIED.** Gates G2, G3 and G7 fail; G1 passes on everything that ran but its holdout leg is missing; G6 needs a reader-only memory measurement.

## 2. Gates

| Gate | Requirement | Result | Status |
|---|---|---|---|
| G1 no wrong payload | 0 in structured + adaptive + holdout | 0 / 1,364,000 (holdout not run) | ✅ for what ran; holdout leg open |
| G2 clean round trip | 100% on S01 at ≥ 6 px/module | S01 recoverable 98.77%; 421 clean misses, all at ≤ 6.7 px/module (§4.1) | ❌ |
| G3 normal use | ≥ 99.9% on recoverable, severity bins 0–1, use-weighted | S01 alone (weight 0.10) is 98.8%; most strata are far lower | ❌ (not close) |
| G4 latency | P95 ≤ 200 ms on the reference iOS device | Device never named. Host P95 at 1080 px: 215 ms (P0.1), 228 ms (P0) | ⚪ not measurable |
| G5 no regression | vs baseline | This run *is* the baseline | ⚪ n/a |
| G6 memory/compute safety | 0 crashes/timeouts, peak ≤ 512 MB | 0 reader faults; peak heap P99 477 MB, **max 551 MB** (reader + simulator in one process); possible unbounded compute behind the stall (§6) | ⚠️ re-measure |
| G7 coverage | 1M structured, ≥ 500k adaptive, ≥ 100k holdout, real device | 1M ✅, 354k adaptive ❌, holdout 0 ❌, device 0 ❌ | ❌ |

## 3. What was run

| Cohort | Cases | Decoded | Wrong | Notes |
|---|---:|---:|---:|---|
| Pilot | 10,000 | 54.86% | 0 | gate passed |
| Structured | 1,000,000 | 50.14% of 900,000 valid | 0 | 20 strata × 50,000, frozen and signed |
| Adaptive, failure-targeted | 350,000 | 22.25% | 0 | 194 weakest (stratum, profile, severity) bins |
| Adaptive, cross-condition | 4,000 of 50,000 | 0.03% | 0 | stalled |
| Adaptive, boundary search | 0 of 100,000 | | | not started |
| Holdout | 0 of 100,000 | | | not started |

The structured mix: 693k wide-camera, 143k budget 1080p, 80k 720p, 71k telephoto and 12k ultrawide frames; 621k ideal, 261k print and 117k display substrates; P0 versions 1–31 evenly; ECC L, M, Q and H evenly, standard and print layouts.

## 4. Where the reader fails, ranked

Ranked by how much recoverable success each fix would unlock and how often the condition occurs in real use. "Recoverable" excludes frames the physics classifier marks unrecoverable, so these are losses a better reader can win back.

### 4.1 Clean symbols near 6 px/module (G2)
* S01 (clean, ideal, no degradation) decodes 98.77% of recoverable cases, not 100%. The 421 misses all sit **at or below 6.7 px/module** (the exported examples are the highest-resolution misses in each cell: 5.9 px/module for P0, 6.7 for P0.1), on frames with zero blur, motion, glare or noise.
* Their recorded localization error is **1.0–1.9 modules**. Some decode when the reader is given the true corners (a corner-fitting miss); others still fail with true corners, so sampling at that density also misregisters.
* In practice the reader's clean floor is about **7 px/module**, while the format declares 6. Either fix the reader at 6–7 px/module (preferred; repair notes 1.4 and 1.5) or raise the declared minimum.
* Repro: the S01 rows in `evidence/counterexamples/`, `./ckt repro <case_id>`.

### 4.2 Uneven light and low contrast: S09, 26.8% recoverable (weakest stratum)
* Even the easiest bin (Michelson contrast 0.6–0.9) decodes only 51.8%. Failures are mostly **NO_BORDER** and **BORDER_MISS**, and 12,604 of them are **localization-limited**: the decoder reads the frame when handed the true corners.
* The reader loses the symbol before it samples a single module. This is the same mechanism as the room-brightness and vignette finding (repair notes 1.9, and the paper OCF-2026-000001): a frame-wide tone reference.
* **Fix:** local adaptive thresholding referenced to the symbol's own frame, finders and keys (1.1), and region-of-interest decoding (1.8 `regionOfInterest`).

### 4.3 Colour designs: S15, 54.6% recoverable
* Localization-limited failures (12,561) outnumber sampling failures (11,117). This is the only stratum where that happens. Even the high-contrast bin (WCAG 7–21:1) decodes only 74.9%.
* It has the same root cause as 4.2: border detection uses luminance that a coloured field or background does not supply well. The same local-threshold fix applies. Also check the luminance conversion (sRGB linearisation before thresholding).

### 4.4 Defocus (S06, 37.1%) and motion blur (S07, 38.5%)
* Both are **sampling-limited** (UNCORRECTABLE dominates; 28,854 and 26,637 sampling-limited vs 2,660 and 3,655 localization-limited). Success drops from about 80% to 16% (defocus) and 24% (motion) by bin 2 (blur 0.3–0.45 module; motion 1–1.5 module).
* Some bin-0 failures are striking: defocus 0.06 px at 12 px/module still fails (with 20° yaw).
* **Fixes:**
  * erasure decoding and soft-decision retries (1.2, 1.3; burst plan options 1 and 2);
  * multi-frame accumulation in the SDK, for motion;
  * for defocus, the capture layer's focus-on-code (2.3).

### 4.5 Perspective: S04, 50.8% recoverable
* Success falls to 12% at 45–60° and 1.5% at 60–72°. **BORDER_MISS** dominates, and 8,797 failures are localization-limited.
* This is the angle and ellipse-constrained-fit item (repair notes 1.7).

### 4.6 Lens distortion and the ultrawide camera
* The **ultrawide device decodes 0.8%** (90 of 11,080).
* S19 isolates the cause: its class 1 (ultrawide barrel distortion, k1 −0.08 to −0.15, code off-centre) decodes **1%**, against 76–82% in class 0 (mild distortion).
* The reader fits a planar homography, which cannot follow a barrel-distorted field.
* **Fixes:**
  * a radial-distortion term in the quad fit, or per-device undistortion from the capture layer;
  * in the SDK, prefer the wide or tele camera and real optical zoom over the ultrawide. The iOS app already does this; the SDK camera layer should too (section 2).
* *New: not in the repair notes yet.*

### 4.7 Finder occlusion, partial crops, glare (cliffs)
| Condition | Success |
|---|---|
| Finder covered 1–3% (S19 class 2) | 59–63% |
| Finder covered 3–6% plus a glare spot (S19 class 3) | **0.1%** |
| Symbol cropped 3–25% out of frame (S16 class 4) | **2.3%**, versus ~81% for any amount of clutter or distractors |
| Glare over 5–10% of the symbol (S10 bin 1) | **1%**; 0% beyond 10% |
| Occlusion 5–10% of the field (S11 bin 1) | 22% |

* These are cliffs, not slopes. A few percent of damage in the wrong place costs the whole read.
* That is what erasure decoding is for. Clipped, occluded and out-of-frame sites become erasures; RS then corrects `2e + f ≤ parity` (repair notes 1.2 and 3.1).
* Finder dependence suggests the reader needs a fallback that localises from the field and keys when one finder is lost.
* *New: the crop and finder-cover cliffs are not in the repair notes yet.*

### 4.8 Density
* P0 versions 25–31 decode 32.9%, against 52% for v1–16. P0.1 holds up better (46.8% at v25–31).
* At v31 a 1080p frame cannot hold the symbol at 6 px/module, so part of this is physics (`below_declared_min_to_fit`).
* **Advice:** in the renderer, steer users to P0.1 and to the lowest version that fits.

### 4.9 Screens, print and rays
| Stratum | Recoverable | What it measures |
|---|---|---|
| S13 screens / moiré | 69.9% | display pixels per module (repair notes 1.6) |
| S12 dot gain | 59.3% | ink spread |
| S18 ray matrix | 65.9% | falls to 0.1% at 0.9–1.3 px per ray slot |

All three are sampling-limited.

### 4.10 What works well
| Stratum | Recoverable |
|---|---|
| S02 payload patterns | 94.6% |
| S05 rotation, any angle (flat across bins) | 94.8% |
| S08 low light | 91.6% |
| S14 JPEG down to quality 10 | 89.6% (71% even at Q 10–30) |
| S16 clutter and distractors (crops aside) | 94.6% |
| S20 combined realistic conditions | 90.0% |

## 5. Format and ECC observations
* **P0.1 beats P0 by 7.3 points** overall (53.8% vs 46.5%) and in 19 of 20 strata (S11 occlusion is the exception, 18.9% vs 18.4%). This is consistent with the build-58 print findings.
* **ECC level helps less than expected** (P0: L 41.2%, H 50.7%; P0.1: L 50.4%, H 56.8%). Most failures happen before RS sees the bytes (localization) or as bursts that exceed the parity because placement scatters them. The burst plan (options 1–4) and higher levels U and X (repair notes 3.5a) only pay off once erasures are used.
* **Hard negatives:**
  * the kinds that must be rejected (random codeword, foreign grid, blank) were rejected 100%;
  * the "must not read wrong" kinds (mirror, splice, t+1, CRC forge, heavy corruption) produced 0 wrong reads. Mirrors that decoded returned the true payload, which the spec allows.

## 6. The stall (fix before any re-run)
* After the failure-targeted pool sealed (16:15 UTC), the cross-condition pool sealed 4 shards in 13 minutes, decoding 1 of 4,000 cases. Then nothing sealed for about 10.5 hours.
* Those cases are the worst ten conditions recombined pairwise, the hardest inputs of the campaign.
* **Likely cause:** one or more cases sent a worker into very long or unbounded compute, and the harness has no per-case timeout to kill it. If so, it is a **G6 finding against the reader** ("unbounded adversarial compute"), not only a harness bug.
* **Before re-running:**
  1. add a per-case wall-clock limit that records a timeout as a result;
  2. regenerate the cross-condition plan from the locked selection (it is deterministic) and run it locally to find the case;
  3. bound the reader's candidate search.
* `check.sh` should also warn when no shard has sealed for over 30 minutes, which would have caught this hours earlier.

## 7. Memory
* The worker's per-case peak heap is P50 50 MB, P99 477 MB, max 551 MB. That is over the 512 MB G6 limit, but the figure includes the simulator's frame buffers.
* Measure the reader alone, wrapping only the decode call. If the reader stays under 512 MB, record that and G6 memory passes. If it doesn't, cap candidate counts and working-image sizes.

## 8. Order of work
1. **Harness:** per-case timeout, stall alarm, reader-only memory measurement (§6, §7).
2. **Reader, localization:** local thresholding (4.2, 4.3), ellipse-constrained fit (4.5), radial distortion (4.6), finder-loss fallback (4.7), the 6–7 px/module clean misses (4.1).
3. **Reader, decoding:** erasure decoding, then soft-decision retries (4.4, 4.7).
4. **SDK capture layer:** focus and exposure on the code, prefer wide or tele, real zoom, region of interest, multi-frame accumulation.
5. **Format (later):** byte-grouped placement and interleaving; levels U and X.
6. **Re-run:**
   * the same locked design on the fixed build, so G5 can compare against this baseline;
   * the full adaptive pool and the 100,000-case holdout;
   * a named reference iOS device, for G4 and the real-device corpus.

The holdout was never opened. Its seed commitment (`5b1ea0cc…`) still holds, so the next run can use it as an untouched holdout.
