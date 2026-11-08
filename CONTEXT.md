# Context: baseline inventory, discrepancies, open issues

Phase 0 inventory of the production repository (SPEC §3), taken 2026-11-07/08.
Everything below was read from source at the pinned commit; nothing was assumed.

## Repositories

| | |
|---|---|
| Spec name | `clearkeysolutions/ckey-test` |
| Actual temporary repository | **`clearkeysolutions/key-test`** (the repository the owner provided; same role) |
| Production repository | `clearkeysolutions/ckey`, public, MIT licence |
| Baseline commit | `ede38f57aa1a20c456b1858f4fccd919e98ef269` · software `0.1.0-build.85` · C ABI version 4 |

## Profiles as the production code defines them

The mandate's P0/P1/P2/P3 map onto the code exactly. One addition: **"P0.1"**, the Format 0.28 print layout of Profile 0.

| Mandate | Code | Format (encoder emits) | FEC | Payload | Field / modules | Image decoder |
|---|---|---|---|---|---|---|
| P0 | scalable, `CKEY_LAYOUT_STANDARD` | 0.27 (`0x1B`) | RS blocks, levels L/M/Q/H (~7/14/22/30%) | v1..v31: 27..4,619 B at M | 29 units, 29..232 modules | `ckey_decode_scalable_gray8` |
| P0.1 | scalable, `CKEY_LAYOUT_PRINT` | 0.28 (`0x1C`), **draft** | same | 7..3,453 B at M | same skeleton, ≥1-module marks | same |
| P1 | locator | 0.14 (`0x0E`) | RS(77,26), t = 25 | 16 B fixed | 29 units, 29 modules | `ckey_decode_gray8` |
| P2 | text | 0.25 (`0x19`) | RS(255,210), t = 22 | 1..200 B | 29 units, 58 modules (0.5-unit pitch) | `ckey_decode_text_gray8` |
| P3 | extended | 0.26 (`0x1A`) | 10 × RS(255,k), 7 tiers | 1..2,048 B | 116 units, 232 modules | `ckey_decode_extended_gray8` |

Every profile protects its frame with CRC-32C (Castagnoli). The harness's independent RS oracle (`runners/native/symbol_factory.h`) reproduces production parity bit for bit for P1 and P2. The selftest checks this, and so does every case that touches FEC.

The canonical geometry this inventory confirmed, and which the harness preserves (it renders with the production renderer only):
* rounded-square border;
* one inboard finder arc per corner, with the top-left arc split at 225° for orientation;
* protected 6 × 6 corner envelopes;
* a Grid Key;
* polar ring matrix of dots and joined arcs;
* radial ray dial with variable lengths and widths (Gray-coded length bits plus a width bit);
* Ring and Ray Keys;
* clear centre;
* whitened, pseudo-randomly placed data sites with declared clearances.

## The production reader loop

The iOS reader (`ios/CKeyReader/CameraScanner.swift`) and the SDK (`sdk/swift/Sources/CKeyScanKit/CKeyDecoder.swift`) run the same loop, all of it inside the C++ core:

1. `ckey_find_borders_gray8` returns up to 6 border candidates, both polarities.
2. For each candidate in order:
   1. refine to `CKEY_FIELD_SCALABLE` and decode P0;
   2. refine to `LOCATOR` and decode P1;
   3. refine to `TEXT` and decode P2;
   4. refine to `EXTENDED` and decode P3.
3. The first `CKEY_OK` wins. A payload is accepted only after RS correction and the CRC-32C check.

`runners/native/scanner_adapter.h` is a call-for-call C++ port of that loop. It adds instrumentation only.

Camera: the reader requests the 4K preset (2160 × 3840 portrait luma) and falls back to 1080p. It runs continuous autofocus and auto-exposure, uses zoom 1.0, and samples at most 20 frames/s. **There is no temporal fusion**: each frame is decoded independently and the first decode is published. So a temporal sequence (S20) in this harness is one scan attempt, and it succeeds on the first decoded frame.

## Declared performance in the production docs (used for gate G2)

* P0: "all 31 versions decode at 6 px per module clean" (spec 0.27).
* P2: "reads clean renders from about 10 px per unit" (= 5 px/module) (docs/OVERVIEW.md).
* P3: "from about 11 px per unit" (= 5.5 px/module).
* P1: **no figure is stated**. The harness assumes 6 px/module (open issue OI-3).

## Discrepancies and how they are handled (none silently changed)

| # | Discrepancy | Handling |
|---|---|---|
| D1 | The mandate names the repo `ckey-test`; the provided repo is `key-test`. | Used `key-test`. Paths and docs say so. |
| D2 | The mandate lists P0..P3; the code also has P0.1 (Format 0.28, draft). | P0 cases are split 50/50 between standard and print layouts, and reported separately in `format_version`. |
| D3 | Formats 0.25–0.28 are all marked "draft" in the spec. | Tested as the current canonical format at the pinned commit. Certification scope will say "draft formats". |
| D4 | P0.1 has no separately declared minimum resolution. | Assumed 6 px/module (same as P0) for G2; owner to confirm (OI-3). |
| D5 | The reader has no multi-frame fusion, although SPEC §6 lists it as a mechanism. | S20 measures the real behaviour (first decoded frame). Fusion would be a proposed change, not something the test assumes. |
| D6 | ckey's own SVG rasterizer (`src/svg_raster.h`) is test-only code. | The harness uses its own independent rasterizer for textures. ckey's rasterizer is the cross-check oracle and the holdout's alternate rendering pipeline. |

## Owner scope decisions (2026-11-08)

* **Only P0 and P0.1 are tested**, across all densities (versions 1–31) and ECC levels. P1/P2 will be deprecated; P3 needs no tests. The harness still contains P1–P3 support (encoder, oracle, selftest). The reader adapter runs the full production loop, so a P0 symbol misdecoded by another profile's decoder would still count as a wrong payload.
* **No 4K capture frames.** Hardening targets ordinary 1080p/720p capture. Note that "4K" referred to the camera frame, not the print, and the shipping iOS reader does request 4K first (`CameraScanner.swift`). `configs/devices.yaml: allow_4k_fallback` re-enables it for dense versions only.
* **Storage:** Cloudflare R2 bucket `ckey-hardening-test` holds the content-addressed shard bundles. git holds `evidence/`: hashes, manifests, reports, counterexample case lines and gzipped SVGs.
* **ML:** build and train it (`./ckt ml`), and store everything needed to retrain.

## Resolution and QR parity (why dense P0 "does not fit" 1080p)

Pixels per module is set by how many modules a symbol has, not by the format family. The capture frame's short side is 1,080 px. With the 0.9 fill used here, the most px/module available for a symbol N module-widths across (quiet margin included) is about 972 / N.

| Symbol | Module-widths across | Max px/module in 1,080 px | Capacity (byte mode, level M) |
|---|---:|---:|---:|
| P0 v1 | 31 | 31 | 27 B |
| P0 v11 | 62 | 15.7 | 228 B |
| P0 v21 | 124 | 7.8 | 1,085 B |
| P0 v31 | 248 | 3.9 | 4,619 B |
| QR v10 (57 + 8 quiet) | 65 | 15.0 | 213 B |
| QR v25 (117 + 8) | 125 | 7.8 | 997 B |
| QR v40 (177 + 8) | 185 | 5.3 | 2,331 B |

So at **equal width, P0 holds about the same as QR** (P0 v21 1,085 B vs QR v25 997 B, both ~124 module-widths). P0 v31 holds twice QR v40's maximum, and no 1080p frame can show either of them at 6 px/module.

The gap to QR is therefore **not** the module count. It is the px/module the reader needs:
* ckey documents 6 px/module as the clean minimum for P0;
* QR readers routinely decode at 2–3 px/module.

Closing that gap is what this campaign measures. S03 sweeps 1.2–7.5 px/module, and every dense version is imaged at whatever 1080p allows. The development observations below already show the main obstacle: corner/quad localization, not the error correction. A direct QR baseline in the same simulator would need zxing-cpp; ckey already has an optional `CKEY_BENCH_ZXING` benchmark. It is not part of this campaign yet (open issue OI-7).

## Open issues needing the owner

* **OI-1 Reference iOS device (gate G4).** None is named, so G4 stays NOT_EVALUATED. Server timings are reported as indicative only.
* **OI-2 Real capture corpus.** None exists yet. Simulated distributions cannot be called "realistic" until compared with one (`certification/qualification/real_device_protocol.md`). Until then the best possible label is SYNTHETICALLY_HARDENED.
* **OI-3 P0.1 declared minimum resolution.** Assumed 6 px/module, as for P0.
* **OI-4 Mirror handling.** It is unspecified whether a mirrored symbol must be rejected or may decode. The harness only requires that it never yields a *different* payload (`NO_WRONG`).
* **OI-5 Push access to `clearkeysolutions/ckey`.** This session had read access only. Fixes are prepared as patches or PRs for human review, never merged automatically.
* **OI-7 QR baseline.** Run QR (zxing-cpp) through the same simulator for a head-to-head px/module floor? This is not in scope until you approve it.
* **OI-6 Gate approval.** `configs/gates.yaml` is PROPOSED. `./ckt lock --approver "NAME"` records your approval before any counted case runs.

## Development observations (NOT campaign results)

These turned up while building and debugging the harness, on a handful of cases. They are listed so they can be checked against the campaign, not as findings.

1. **P3 localization on clean renders.** On mathematically perfect P3 rasters at 6–7.5 px/module (12–15 px/unit, above the documented 11), several payloads fail. The cause is that `ckey_refine_quad_gray8(CKEY_FIELD_EXTENDED)` converges 15–28 px away from the true field corners. Given the true corners, `ckey_decode_extended_gray8` decodes the same images with zero corrections. In one case the calibration score at the wrong quad (210) was higher than at the truth (199), so the refinement objective itself is biased.
2. **P1 at 6 px/module, rotated 90°, perfect raster.** The reader fails (UNCORRECTABLE) with a 5.9 px (about one module) corner error, while the true quad decodes. This is the same localization mechanism as observation 1.
3. **Sanitizers.** The ASan + UBSan build of the core plus harness passed the selftest with no findings.

The harness records `gt_quad_decodes` on every failure. That field separates localization-limited failures from sampling- or decoding-limited ones in every report.
