# Conformance (SPEC §11): classes and vector export

This campaign separates four classes of conformance. Proposed vectors are exported from campaign evidence. They are frozen and moved into `clearkeysolutions/ckey` (`tests/vectors/`) only through a reviewed PR.

| Class | Question | Evidence in this repo | Proposed vectors |
|---|---|---|---|
| A. Renderer conformance | Does an encoder/renderer produce the normative geometry and bytes? | The independent RS oracle vs production parity (`rs_verified`); rasterizer cross-checks (S01 `crosscheck`, P0 native raster) | Per-profile byte frames plus SVG hashes for representative payload patterns (S02) |
| B. Static decoder conformance | Given an image and the true field quad, does the decoder return exactly the payload? | `gt_quad_decodes` on every failure; the S01 ideal rasters | Ideal rasters at the declared minimum px/module, with expected payloads |
| C. Live camera reader qualification | Does the full-frame reader (border → refine → decode) succeed under the defined capture conditions? | The structured, adaptive and holdout reports; boundary search | Minimal counterexamples (frames + case lines) per failure class |
| D. Cross-platform interoperability | Do the C++, Swift, Kotlin, .NET and Web SDK readers agree? | Not covered here. The SDKs call the same core, but their wrappers differ. | Class B/C vectors replayed through each SDK's test runner (future work) |

## Rules for any proposed threshold

Each threshold is a minimum print or display module size, a contrast floor, a finder exclusion or a sampling tolerance. It must state:
* its physical justification;
* the observed margin (from boundary search);
* its Wilson/exact confidence interval;
* backward compatibility;
* its versioning impact.

The symbol format is never changed to fit a deficient scanner without mathematical proof, a versioned spec and owner review.

`adapter_parity.md` documents why `runners/native/scanner_adapter.h` is equivalent to the production reader loop.
