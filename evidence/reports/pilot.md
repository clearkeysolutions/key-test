# Pilot (Phase 2, 10,000 cases; not counted)

Cohort `pilot` · 10,000 completed cases · generated 2026-11-08 06:18 UTC

> Simulated, host-side evidence about the production CKey core. Not a certification; see `certification/qualification/README.md`.

## Safety: accepted wrong payloads

**Total wrong payloads: 0** · one-sided 95% upper bound on the rate: 3.00e-04 (assumes independence)

| Variant | Cases | Wrong | 95% upper rate |
|---|---:|---:|---:|
| P0 | 5,000 | 0 | 5.99e-04 |
| P0.1 | 5,000 | 0 | 5.99e-04 |

## Valid-symbol decode success

* All valid cases: 49.84% [48.81, 50.88] (n=9,000)
* Physically recoverable only: 69.24% [67.86, 70.59] (n=4,402)
* Cluster-robust (payload families): n_eff 3,244, design effect 2.77, 95% [48.13, 51.57]%

| Stratum | Name | All valid | Recoverable only |
|---|---|---|---|
| S01 | Clean, ideal reference and exact round-trip | 90.67% [87.62, 93.02] (n=450) | 99.16% [97.57, 99.72] (n=359) |
| S02 | Payload length, density, pathological bit patterns | 81.11% [77.24, 84.46] (n=450) | 92.57% [88.80, 95.14] (n=269) |
| S03 | Symbol scale, downsampling, subpixel phase | 57.78% [53.17, 62.26] (n=450) | 74.80% [69.02, 79.81] (n=246) |
| S04 | Perspective, skew, nonplanar warping | 38.89% [34.50, 43.47] (n=450) | 57.21% [50.53, 63.64] (n=215) |
| S05 | Rotation, mirror/reflection rejection, finder orientation | 77.56% [73.48, 81.17] (n=450) | 92.12% [88.02, 94.89] (n=241) |
| S06 | Gaussian/optical defocus and lens PSF | 30.67% [26.58, 35.08] (n=450) | 37.63% [32.29, 43.28] (n=295) |
| S07 | Directional and rolling-shutter motion blur | 32.89% [28.71, 37.36] (n=450) | 40.28% [34.78, 46.03] (n=288) |
| S08 | Low light, photon/read noise, high ISO | 63.56% [59.01, 67.87] (n=450) | 94.57% [87.90, 97.66] (n=92) |
| S09 | Contrast, uneven illumination, shadows | 20.44% [16.98, 24.41] (n=450) | 25.00% [20.76, 29.78] (n=352) |
| S10 | Specular glare, saturation, highlights | 4.67% [3.07, 7.03] (n=450) | 75.00% [46.77, 91.11] (n=12) |
| S11 | Occlusion, tears, scratches, missing regions | 18.22% [14.93, 22.05] (n=450) | 77.78% [54.79, 91.00] (n=18) |
| S12 | Print dot gain, ink bleed, halftone, paper | 42.89% [38.40, 47.50] (n=450) | 55.78% [49.59, 61.79] (n=251) |
| S13 | Screen pixels, RGB subpixels, moire, refresh artifacts | 47.56% [42.98, 52.17] (n=450) | 67.19% [61.19, 72.68] (n=253) |
| S14 | Compression, sharpening, denoising, ISP artifacts | 74.89% [70.68, 78.67] (n=450) | 91.91% [86.10, 95.42] (n=136) |
| S15 | Color foreground/background, gamma, transparency | 47.11% [42.54, 51.73] (n=450) | 55.87% [50.63, 60.99] (n=349) |
| S16 | Clutter, distractors, partial crops, multiple codes | 66.00% [61.50, 70.22] (n=450) | 95.96% [92.51, 97.86] (n=223) |
| S17 | Ring matrix dot/arc and radial sampling stress | 67.78% [63.33, 71.93] (n=450) | 84.69% [80.13, 88.36] (n=294) |
| S18 | Ray matrix length/weight, angular aliasing | 41.78% [37.31, 46.39] (n=450) | 67.96% [61.31, 73.95] (n=206) |
| S19 | Grid matrix, finder-safe-zone and corner localization | 26.44% [22.58, 30.71] (n=450) | 73.03% [63.00, 81.16] (n=89) |
| S20 | Combined realistic conditions and temporal frame sequences | 66.00% [61.50, 70.22] (n=450) | 89.25% [84.39, 92.73] (n=214) |

### By variant (P0 = Format 0.27 standard, P0.1 = Format 0.28 print)

| Variant | Success |
|---|---|
| P0 | 46.96% [45.50, 48.42] (n=4,500) |
| P0.1 | 52.73% [51.27, 54.19] (n=4,500) |

### By density (P0 version band) and variant

| Variant / band | Success |
|---|---|
| P0.1/v1-8 | 55.16% [52.38, 57.91] (n=1,240) |
| P0.1/v17-24 | 53.49% [50.73, 56.23] (n=1,260) |
| P0.1/v25-31 | 44.13% [40.95, 47.36] (n=920) |
| P0.1/v9-16 | 56.39% [53.41, 59.32] (n=1,080) |
| P0/v1-8 | 51.89% [49.08, 54.68] (n=1,220) |
| P0/v17-24 | 43.25% [40.47, 46.07] (n=1,200) |
| P0/v25-31 | 35.71% [32.78, 38.76] (n=980) |
| P0/v9-16 | 55.55% [52.59, 58.46] (n=1,100) |

### By stratum and severity bin (0 = easiest)

| Stratum | b0 | b1 | b2 | b3 | b4 |
|---|---|---|---|---|---|
| S01 | 91.1% | 87.8% | 93.3% | 90.0% | 91.1% |
| S02 | 81.1% | 78.9% | 86.7% | 78.9% | 80.0% |
| S03 | 86.7% | 78.9% | 74.4% | 35.6% | 13.3% |
| S04 | 65.6% | 56.7% | 60.0% | 11.1% | 1.1% |
| S05 | 72.2% | 76.7% | 83.3% | 77.8% | 77.8% |
| S06 | 82.2% | 53.3% | 17.8% | 0.0% | 0.0% |
| S07 | 76.7% | 57.8% | 21.1% | 8.9% | 0.0% |
| S08 | 80.0% | 73.3% | 74.4% | 63.3% | 26.7% |
| S09 | 50.0% | 32.2% | 15.6% | 4.4% | 0.0% |
| S10 | 22.2% | 1.1% | 0.0% | 0.0% | 0.0% |
| S11 | 53.3% | 26.7% | 5.6% | 2.2% | 3.3% |
| S12 | 68.9% | 60.0% | 51.1% | 24.4% | 10.0% |
| S13 | 67.8% | 67.8% | 51.1% | 42.2% | 8.9% |
| S14 | 82.2% | 72.2% | 73.3% | 74.4% | 72.2% |
| S15 | 72.2% | 65.6% | 48.9% | 31.1% | 17.8% |
| S16 | 82.2% | 77.8% | 81.1% | 84.4% | 4.4% |
| S17 | 82.2% | 70.0% | 76.7% | 64.4% | 45.6% |
| S18 | 74.4% | 73.3% | 47.8% | 13.3% | 0.0% |
| S19 | 76.7% | 0.0% | 55.6% | 0.0% | 0.0% |
| S20 | 81.1% | 71.1% | 65.6% | 62.2% | 50.0% |

## Failure mechanisms (valid symbols)

| Stratum | Mechanisms | GT-quad triage (localization / sampling-decode) |
|---|---|---|
| S01 | BORDER_MISS 27, NO_BORDER 8, UNCORRECTABLE 7 | 3 / 39 |
| S02 | UNCORRECTABLE 48, BORDER_MISS 33, NO_BORDER 4 | 18 / 67 |
| S03 | UNCORRECTABLE 124, NO_BORDER 43, BORDER_MISS 23 | 54 / 136 |
| S04 | BORDER_MISS 117, UNCORRECTABLE 99, NO_BORDER 59 | 86 / 189 |
| S05 | UNCORRECTABLE 60, BORDER_MISS 35, NO_BORDER 6 | 21 / 80 |
| S06 | UNCORRECTABLE 224, BORDER_MISS 44, NO_BORDER 44 | 21 / 291 |
| S07 | UNCORRECTABLE 207, BORDER_MISS 58, NO_BORDER 37 | 39 / 263 |
| S08 | UNCORRECTABLE 94, BORDER_MISS 62, NO_BORDER 8 | 53 / 111 |
| S09 | NO_BORDER 176, BORDER_MISS 117, UNCORRECTABLE 65 | 126 / 232 |
| S10 | UNCORRECTABLE 319, BORDER_MISS 98, NO_BORDER 12 | 4 / 425 |
| S11 | UNCORRECTABLE 248, BORDER_MISS 101, NO_BORDER 19 | 20 / 348 |
| S12 | UNCORRECTABLE 190, BORDER_MISS 42, NO_BORDER 25 | 54 / 203 |
| S13 | UNCORRECTABLE 158, NO_BORDER 40, BORDER_MISS 38 | 59 / 177 |
| S14 | UNCORRECTABLE 70, BORDER_MISS 26, NO_BORDER 17 | 21 / 92 |
| S15 | NO_BORDER 108, UNCORRECTABLE 77, BORDER_MISS 53 | 134 / 104 |
| S16 | BORDER_MISS 89, UNCORRECTABLE 40, NO_BORDER 24 | 14 / 139 |
| S17 | UNCORRECTABLE 80, BORDER_MISS 50, NO_BORDER 15 | 55 / 90 |
| S18 | UNCORRECTABLE 163, BORDER_MISS 50, NO_BORDER 49 | 44 / 218 |
| S19 | BORDER_MISS 137, UNCORRECTABLE 137, NO_BORDER 57 | 10 / 321 |
| S20 | UNCORRECTABLE 92, BORDER_MISS 46, NO_BORDER 15 | 25 / 128 |

localization_limited: the production decoder reads the frame when given the true field corners, so the failure is in border finding / quad refinement; otherwise sampling or decoding is limiting.

## Hard negatives

| Kind / profile | Cases | Rejected | Wrong |
|---|---:|---:|---:|
| crc_forge/P0 | 95 | 95 | 0 |
| crc_forge/P0.1 | 95 | 95 | 0 |
| heavy_corruption/P0 | 100 | 100 | 0 |
| heavy_corruption/P0.1 | 100 | 100 | 0 |
| mirror/P0 | 10 | 4 | 0 |
| mirror/P0.1 | 10 | 3 | 0 |
| random_codeword/P0 | 100 | 100 | 0 |
| random_codeword/P0.1 | 100 | 100 | 0 |
| splice/P0 | 95 | 90 | 0 |
| splice/P0.1 | 95 | 90 | 0 |
| t_plus_one/P0 | 100 | 100 | 0 |
| t_plus_one/P0.1 | 100 | 100 | 0 |

## Physical recoverability (valid symbols)

| Class | Cases | Success |
|---|---:|---:|
| MARGINAL | 1,159 | 498 |
| RECOVERABLE | 4,402 | 3,048 |
| UNRECOVERABLE | 3,439 | 940 |

## Host latency (indicative only; not gate G4)

| Profile / frame | n | P50 ms | P95 ms | P99 ms |
|---|---:|---:|---:|---:|
| P0.1/1080w | 4,084 | 45.8 | 220.8 | 305.9 |
| P0.1/360w | 1 | 22.2 | 22.2 | 22.2 |
| P0.1/480w | 2 | 31.5 | 40.7 | 41.5 |
| P0.1/540w | 56 | 34.4 | 162.9 | 193.4 |
| P0.1/720w | 614 | 32.7 | 160.2 | 212.8 |
| P0.1/810w | 243 | 35.9 | 177.6 | 237.6 |
| P0/1080w | 4,355 | 49.5 | 229.1 | 321.8 |
| P0/360w | 2 | 28.0 | 30.0 | 30.2 |
| P0/480w | 1 | 47.3 | 47.3 | 47.3 |
| P0/540w | 75 | 28.6 | 117.4 | 140.0 |
| P0/720w | 299 | 35.4 | 166.6 | 300.9 |
| P0/810w | 268 | 46.8 | 250.5 | 324.6 |

Peak per-case heap: P50 50 MB, P99 393 MB, max 528 MB. Reader faults: none.

## Oracle checks

* Renderer cross-check (S01): n=500, max disagreement 0.0
* Independent RS vs production parity: 10,000 verified, 0 mismatches

## Quota

```
{
 "S01": 500,
 "S02": 500,
 "S03": 500,
 "S04": 500,
 "S05": 500,
 "S06": 500,
 "S07": 500,
 "S08": 500,
 "S09": 500,
 "S10": 500,
 "S11": 500,
 "S12": 500,
 "S13": 500,
 "S14": 500,
 "S15": 500,
 "S16": 500,
 "S17": 500,
 "S18": 500,
 "S19": 500,
 "S20": 500
}
```
