# Adapter parity: harness reader loop vs production

Reference: `sdk/swift/Sources/CKeyScanKit/CKeyDecoder.swift` at `ede38f5`, which documents itself as the pipeline of `ios/CKeyReader/CameraScanner.swift`.

| Step | Swift (production) | C++ adapter (`scanner_adapter.h`) |
|---|---|---|
| Candidates | `ckey_find_borders_gray8(..., maxCandidates = 6)` | `ckey_find_borders_gray8(..., kMaxCandidates = 6)` |
| Order per candidate | P0 (`refine SCALABLE` → `decode_scalable_gray8`), then `decodeLegacy`: P1, P2, P3 | identical order |
| Refine acceptance | `score >= 0` | `score >= 0` |
| P1 capacity | `CKEY_LOCATOR_PAYLOAD_BYTES` | same |
| P0/P2/P3 output buffer | 65,535 bytes; accept only if `length <= count` | same |
| Result | first `CKEY_OK` | first `CKEY_OK` |

Differences: none in the calls or the acceptance rule. The adapter records refine scores, quads, statuses and diagnostics. The iOS `CameraScanner` additionally refines all four fields before decoding P0 (it computes the quads earlier). That changes no result, because refinement is a pure function of the frame and the border. A change to either file at a new ckey commit requires re-checking this table before locking a new baseline.
