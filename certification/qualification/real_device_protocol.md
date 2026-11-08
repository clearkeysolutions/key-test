# Real-device validation protocol (SPEC §5, §13 item 7; gates G4, G7)

This cannot run on a server. It needs the reference iPhone (to be named by the owner: open issue OI-1).

## Corpus capture (validates the simulator before any realism claim)
1. Print symbols from the generator (`./ckt repro <case_id>` writes the SVG-derived frames; print the production SVGs at the stated mm sizes) on matte and glossy paper at 300/600/1200 dpi. Also show them on 3 phone or laptop displays.
2. For each condition cell (distance, tilt, light level in lux, motion), record ≥30 s of 4K frames with the CKey reader's capture settings: continuous AF/AE, zoom 1.0. Log lux, distance and tilt with a meter or fixture.
3. Extract every 5th frame. Label the ground-truth payload. Store the frames with checksums under `campaign/real_device/` (not in git).

## Comparison with simulation
* Match each capture cell to simulator cases with the same physical parameters. Compare distributions of the following, with a two-sample KS test per statistic:
  * measured px/module;
  * edge spread (blur);
  * noise σ at mid-grey;
  * saturation fraction.
* Simulator parameters that disagree are recalibrated in a new campaign. Locked results are not edited.

## Device performance (G4)
* Single-frame decode time: instrument `CKeyDecoder.decode` (signposts). Report P50/P95/P99 over ≥10,000 frames covering all profiles, on the reference device, at 25 °C and after 10 minutes of continuous scanning (thermal).
* End-to-end time-to-success from "camera ready" to "payload published", per condition cell.

## Result format
One JSON line per real attempt, in the same fields as `schemas/result.json` with `cohort: "real_device"`. Then `./ckt review` can include them once ingested (future work: `ckt ingest-real`).
