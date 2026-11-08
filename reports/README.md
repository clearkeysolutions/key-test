# reports/

Small, human-readable summaries that are safe to commit. Full machine-readable reports are written to `campaign/reports/` by the campaign (not in git) and archived with `scripts/archive.sh`:

| File | Stage |
|---|---|
| `preflight.json` | preflight benchmark, ETA, consistency |
| `plan_validation.json` | VALIDATE_ARTIFACT quota reconciliation |
| `pilot.md/json`, `pilot_gate.json` | Phase 2 pilot (not counted) |
| `structured_1m.md/json` + `.freeze.json(.sig)` | REPORT_1M, frozen before adaptation |
| `counterexamples_structured.json` | categorized minimal counterexamples |
| `adaptive_selection.json` | weakness scores and allocation, published before adaptive execution |
| `adaptive_500k.md/json` + `.freeze.json(.sig)` | EXECUTE_500K (conditional estimates; never pooled with the baseline) |
| `minimal_counterexamples.json` | boundary-search tracks: last pass, first fail, and the conditions needed |
| `regression_<label>.json`, `holdout.md/json` | candidate evaluation |
| `gates.json` | G1–G7 and the assigned label |
| `security.json` + `security/` | sanitizer and fuzz evidence |

No campaign results are committed here yet: the campaign has not run.
