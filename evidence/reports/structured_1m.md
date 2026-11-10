# Structured campaign: 1,000,000 cases (frozen)

Cohort `structured` · 1,000,000 completed cases · generated 2026-11-09 07:16 UTC

> Simulated, host-side evidence about the production CKey core. Not a certification; see `certification/qualification/README.md`.

## Safety: accepted wrong payloads

**Total wrong payloads: 0** · one-sided 95% upper bound on the rate: 3.00e-06 (assumes independence)

| Variant | Cases | Wrong | 95% upper rate |
|---|---:|---:|---:|
| P0 | 500,000 | 0 | 5.99e-06 |
| P0.1 | 500,000 | 0 | 5.99e-06 |

## Valid-symbol decode success

* All valid cases: 50.14% [50.04, 50.25] (n=900,000)
* Physically recoverable only: 68.93% [68.79, 69.07] (n=430,176)
* Cluster-robust (payload families): n_eff 309,400, design effect 2.91, 95% [49.97, 50.32]%

| Stratum | Name | All valid | Recoverable only |
|---|---|---|---|
| S01 | Clean, ideal reference and exact round-trip | 89.86% [89.58, 90.14] (n=45,000) | 98.77% [98.64, 98.88] (n=34,136) |
| S02 | Payload length, density, pathological bit patterns | 80.66% [80.30, 81.03] (n=45,000) | 94.62% [94.34, 94.90] (n=25,241) |
| S03 | Symbol scale, downsampling, subpixel phase | 56.83% [56.38, 57.29] (n=45,000) | 76.77% [76.23, 77.30] (n=24,019) |
| S04 | Perspective, skew, nonplanar warping | 36.11% [35.66, 36.55] (n=45,000) | 50.76% [50.09, 51.44] (n=21,095) |
| S05 | Rotation, mirror/reflection rejection, finder orientation | 80.33% [79.96, 80.70] (n=45,000) | 94.76% [94.48, 95.03] (n=25,038) |
| S06 | Gaussian/optical defocus and lens PSF | 29.97% [29.55, 30.39] (n=45,000) | 37.06% [36.50, 37.62] (n=28,401) |
| S07 | Directional and rolling-shutter motion blur | 32.68% [32.25, 33.12] (n=45,000) | 38.46% [37.90, 39.03] (n=28,492) |
| S08 | Low light, photon/read noise, high ISO | 65.90% [65.46, 66.33] (n=45,000) | 91.57% [90.99, 92.12] (n=9,364) |
| S09 | Contrast, uneven illumination, shadows | 21.76% [21.38, 22.14] (n=45,000) | 26.81% [26.35, 27.27] (n=35,330) |
| S10 | Specular glare, saturation, highlights | 6.28% [6.06, 6.51] (n=45,000) | 82.86% [81.17, 84.42] (n=2,071) |
| S11 | Occlusion, tears, scratches, missing regions | 18.62% [18.27, 18.98] (n=45,000) | 83.28% [81.58, 84.85] (n=2,003) |
| S12 | Print dot gain, ink bleed, halftone, paper | 46.40% [45.94, 46.86] (n=45,000) | 59.28% [58.69, 59.88] (n=26,289) |
| S13 | Screen pixels, RGB subpixels, moire, refresh artifacts | 49.36% [48.90, 49.82] (n=45,000) | 69.92% [69.34, 70.50] (n=24,254) |
| S14 | Compression, sharpening, denoising, ISP artifacts | 75.11% [74.71, 75.50] (n=45,000) | 89.60% [89.06, 90.12] (n=12,906) |
| S15 | Color foreground/background, gamma, transparency | 47.38% [46.92, 47.84] (n=45,000) | 54.57% [54.04, 55.09] (n=34,659) |
| S16 | Clutter, distractors, partial crops, multiple codes | 65.11% [64.67, 65.55] (n=45,000) | 94.57% [94.26, 94.87] (n=20,661) |
| S17 | Ring matrix dot/arc and radial sampling stress | 68.38% [67.95, 68.81] (n=45,000) | 83.27% [82.84, 83.70] (n=28,526) |
| S18 | Ray matrix length/weight, angular aliasing | 40.46% [40.01, 40.92] (n=45,000) | 65.86% [65.19, 66.52] (n=19,475) |
| S19 | Grid matrix, finder-safe-zone and corner localization | 28.25% [27.83, 28.66] (n=45,000) | 74.81% [73.91, 75.68] (n=9,165) |
| S20 | Combined realistic conditions and temporal frame sequences | 63.39% [62.94, 63.83] (n=45,000) | 89.99% [89.56, 90.41] (n=19,051) |

### By variant (P0 = Format 0.27 standard, P0.1 = Format 0.28 print)

| Variant | Success |
|---|---|
| P0 | 46.48% [46.33, 46.62] (n=450,000) |
| P0.1 | 53.81% [53.66, 53.95] (n=450,000) |

### By density (P0 version band) and variant

| Variant / band | Success |
|---|---|
| P0.1/v1-8 | 55.58% [55.29, 55.87] (n=113,460) |
| P0.1/v17-24 | 54.59% [54.31, 54.88] (n=117,180) |
| P0.1/v25-31 | 46.75% [46.44, 47.05] (n=102,320) |
| P0.1/v9-16 | 57.48% [57.20, 57.77] (n=117,040) |
| P0/v1-8 | 51.97% [51.69, 52.26] (n=116,100) |
| P0/v17-24 | 47.17% [46.88, 47.46] (n=116,100) |
| P0/v25-31 | 32.95% [32.67, 33.24] (n=101,660) |
| P0/v9-16 | 52.12% [51.84, 52.41] (n=116,140) |

### By stratum and severity bin (0 = easiest)

| Stratum | b0 | b1 | b2 | b3 | b4 |
|---|---|---|---|---|---|
| S01 | 90.3% | 90.2% | 89.9% | 89.6% | 89.4% |
| S02 | 80.7% | 80.4% | 80.6% | 80.3% | 81.4% |
| S03 | 85.3% | 81.8% | 68.3% | 38.3% | 10.5% |
| S04 | 58.2% | 58.7% | 49.9% | 12.3% | 1.5% |
| S05 | 81.0% | 80.0% | 80.1% | 79.6% | 81.0% |
| S06 | 80.4% | 52.9% | 15.8% | 0.8% | 0.0% |
| S07 | 78.1% | 55.7% | 24.4% | 5.2% | 0.1% |
| S08 | 80.6% | 80.4% | 77.8% | 63.8% | 26.8% |
| S09 | 51.8% | 30.8% | 17.0% | 8.3% | 0.9% |
| S10 | 30.4% | 1.0% | 0.0% | 0.0% | 0.0% |
| S11 | 57.2% | 22.9% | 6.5% | 3.6% | 2.8% |
| S12 | 68.3% | 67.8% | 54.6% | 30.5% | 10.8% |
| S13 | 71.5% | 70.8% | 58.0% | 34.6% | 11.9% |
| S14 | 77.6% | 76.6% | 75.5% | 74.7% | 71.1% |
| S15 | 74.9% | 63.4% | 48.8% | 33.1% | 16.6% |
| S16 | 80.5% | 80.9% | 81.0% | 80.9% | 2.3% |
| S17 | 74.8% | 74.5% | 73.6% | 70.1% | 48.9% |
| S18 | 73.2% | 67.9% | 46.4% | 14.7% | 0.1% |
| S19 | 79.1% | 1.1% | 60.8% | 0.1% | 0.1% |
| S20 | 71.7% | 69.7% | 65.1% | 59.0% | 51.5% |

## Failure mechanisms (valid symbols)

| Stratum | Mechanisms | GT-quad triage (localization / sampling-decode) |
|---|---|---|
| S01 | BORDER_MISS 2506, NO_BORDER 1159, UNCORRECTABLE 898 | 215 / 4348 |
| S02 | UNCORRECTABLE 4836, BORDER_MISS 2888, NO_BORDER 977 | 1919 / 6782 |
| S03 | UNCORRECTABLE 12912, NO_BORDER 3628, BORDER_MISS 2885 | 4123 / 15302 |
| S04 | BORDER_MISS 12971, UNCORRECTABLE 9541, NO_BORDER 6240 | 8797 / 19955 |
| S05 | UNCORRECTABLE 4572, BORDER_MISS 3438, NO_BORDER 841 | 1830 / 7021 |
| S06 | UNCORRECTABLE 22359, BORDER_MISS 5104, NO_BORDER 4051 | 2660 / 28854 |
| S07 | UNCORRECTABLE 21326, BORDER_MISS 5838, NO_BORDER 3128 | 3655 / 26637 |
| S08 | UNCORRECTABLE 9036, BORDER_MISS 5620, NO_BORDER 691 | 4879 / 10468 |
| S09 | NO_BORDER 16402, BORDER_MISS 12471, UNCORRECTABLE 6336 | 12604 / 22605 |
| S10 | UNCORRECTABLE 29813, BORDER_MISS 10312, NO_BORDER 2047 | 237 / 41935 |
| S11 | UNCORRECTABLE 25892, BORDER_MISS 9255, NO_BORDER 1473 | 1793 / 34827 |
| S12 | UNCORRECTABLE 18467, BORDER_MISS 3916, NO_BORDER 1737 | 4158 / 19962 |
| S13 | UNCORRECTABLE 15387, NO_BORDER 3898, BORDER_MISS 3502 | 4942 / 17845 |
| S14 | UNCORRECTABLE 7226, BORDER_MISS 2541, NO_BORDER 1435 | 2511 / 8691 |
| S15 | NO_BORDER 9815, UNCORRECTABLE 8240, BORDER_MISS 5623 | 12561 / 11117 |
| S16 | BORDER_MISS 9368, UNCORRECTABLE 3964, NO_BORDER 2368 | 1696 / 14004 |
| S17 | UNCORRECTABLE 7974, BORDER_MISS 5049, NO_BORDER 1206 | 5093 / 9136 |
| S18 | UNCORRECTABLE 17320, BORDER_MISS 5257, NO_BORDER 4215 | 5096 / 21696 |
| S19 | UNCORRECTABLE 13490, BORDER_MISS 13418, NO_BORDER 5381 | 797 / 31492 |
| S20 | UNCORRECTABLE 8889, BORDER_MISS 5577, NO_BORDER 2009 | 2741 / 13734 |

localization_limited: the production decoder reads the frame when given the true field corners, so the failure is in border finding / quad refinement; otherwise sampling or decoding is limiting.

## Hard negatives

| Kind / profile | Cases | Rejected | Wrong |
|---|---:|---:|---:|
| blank/P0 | 6,065 | 6,065 | 0 |
| blank/P0.1 | 6,065 | 6,065 | 0 |
| crc_forge/P0 | 6,165 | 6,165 | 0 |
| crc_forge/P0.1 | 6,165 | 6,165 | 0 |
| foreign_grid/P0 | 6,065 | 6,065 | 0 |
| foreign_grid/P0.1 | 6,065 | 6,065 | 0 |
| heavy_corruption/P0 | 6,165 | 6,165 | 0 |
| heavy_corruption/P0.1 | 6,165 | 6,165 | 0 |
| mirror/P0 | 7,140 | 4,280 | 0 |
| mirror/P0.1 | 7,140 | 3,787 | 0 |
| random_codeword/P0 | 6,165 | 6,165 | 0 |
| random_codeword/P0.1 | 6,165 | 6,165 | 0 |
| splice/P0 | 6,070 | 5,904 | 0 |
| splice/P0.1 | 6,070 | 5,899 | 0 |
| t_plus_one/P0 | 6,165 | 6,165 | 0 |
| t_plus_one/P0.1 | 6,165 | 6,165 | 0 |

## Physical recoverability (valid symbols)

| Class | Cases | Success |
|---|---:|---:|
| MARGINAL | 116,069 | 51,970 |
| RECOVERABLE | 430,176 | 296,526 |
| UNRECOVERABLE | 353,755 | 102,786 |

## Host latency (indicative only; not gate G4)

| Profile / frame | n | P50 ms | P95 ms | P99 ms |
|---|---:|---:|---:|---:|
| P0.1/1080w | 426,712 | 46.4 | 214.6 | 272.7 |
| P0.1/360w | 288 | 23.2 | 80.1 | 118.2 |
| P0.1/480w | 239 | 25.0 | 96.1 | 134.8 |
| P0.1/540w | 5,704 | 29.5 | 138.2 | 195.7 |
| P0.1/720w | 40,780 | 33.0 | 165.1 | 217.5 |
| P0.1/810w | 26,277 | 38.1 | 180.4 | 246.2 |
| P0/1080w | 427,056 | 50.1 | 227.6 | 292.0 |
| P0/360w | 292 | 23.4 | 94.5 | 140.6 |
| P0/480w | 264 | 28.4 | 110.0 | 213.8 |
| P0/540w | 5,563 | 31.0 | 169.3 | 219.0 |
| P0/720w | 40,610 | 34.0 | 174.8 | 233.0 |
| P0/810w | 26,215 | 41.9 | 203.0 | 271.5 |

Peak per-case heap: P50 50 MB, P99 477 MB, max 551 MB. Reader faults: none.

## Oracle checks

* Renderer cross-check (S01): n=49,380, max disagreement 0.463619
* Independent RS vs production parity: 1,000,000 verified, 0 mismatches

## Quota

```
{
 "S01": 50000,
 "S02": 50000,
 "S03": 50000,
 "S04": 50000,
 "S05": 50000,
 "S06": 50000,
 "S07": 50000,
 "S08": 50000,
 "S09": 50000,
 "S10": 50000,
 "S11": 50000,
 "S12": 50000,
 "S13": 50000,
 "S14": 50000,
 "S15": 50000,
 "S16": 50000,
 "S17": 50000,
 "S18": 50000,
 "S19": 50000,
 "S20": 50000
}
```
