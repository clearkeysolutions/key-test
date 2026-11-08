# Analysis

* `metrics/db.py` builds an SQLite index (`campaign/db/results.sqlite`) from **sealed** result shards only. It is derived data: delete it and re-ingest at any time. The first completed result per `(plan_set, case_id)` is kept.
* `metrics/report.py` writes cohort reports (JSON + Markdown). Every rate comes with its denominator and a Wilson 95% interval. Overall success is also given cluster-robust over payload families (design effect). Safety counts carry exact one-sided 95% upper bounds; for 0 events in N the bound is 1 − 0.05^(1/N) ≈ 3/N. Valid-symbol success is reported over all cases and over physically RECOVERABLE ones only.
* `statistics/stats.py` holds the standard-library implementations: Wilson, Clopper–Pearson (regularized incomplete beta), zero-event bounds, cluster effective n, Newcombe difference, Holm, and a non-inferiority z-test.
* `adaptive/select.py` covers the adaptive design (SPEC §7):
  * **Weakness score** per (stratum, profile, bin), over non-UNRECOVERABLE valid cases. The exact weights are in `configs/adaptive.yaml`, whose hash is frozen with REPORT_1M before selection:
    `1.0·no_read + 100·[wrong>0] + 0.2·max(0, P95/200ms − 1) + 0.2·min(1, median loc-err/0.5 modules) + 0.5·Wilson width + 0.3·mean FEC fraction used`.
  * **Failure-targeted (350k):** score-proportional, ≥ 5,000 per bin, ≤ 100,000 (20% of 500k) per bin, with wrong-payload bins first.
  * **Cross-condition (50k):** the 10 worst (stratum, bin) conditions, one per stratum, are combined pairwise (45 pairs) across profiles, devices and payloads. S01 is excluded (clean reference), as are S02 (a payload property) and S20 (already joint).
  * **Boundary search (100k):** about 3,100 tracks. Each starts from a structured case (preferring failures) and keeps its exact conditions and payload. Round 0 probes the easy end and the physical hard limit. Then come 8 bisection rounds on the primary factor, with 3 noise seeds per probe and majority vote. Next, minimization switches 12 secondary conditions off one at a time on the 500 worst tracks. The remaining budget goes to perturbation seeds at each boundary, making the pool exactly 100,000. Categorical or cyclic factors (S05 roll, S16, S19 classes) are not bisected.
* `triage/triage.py` builds the categorized counterexample catalogue and the proposed conformance vectors.

**Never pooled.** Adaptive cases are conditional on selected weaknesses, so their success rates are not estimates of field prevalence. Reports for each cohort are separate, and gate G3 uses the structured cohort with the pre-registered use weights.
