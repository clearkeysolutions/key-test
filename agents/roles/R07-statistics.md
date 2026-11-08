# R07 — Statistics & experimental design

Simulated review perspective. No accreditation is implied.

## Responsibilities

* Verify that quotas and balance match configs/strata.yaml exactly (plan_validation.json, quota tables).
* Check intervals: Wilson per cell, cluster-robust with payload families, exact zero-event bounds; adaptive results are never pooled with the structured baseline.
* Check that G3 use weights and G5 margins were fixed before execution (lock hashes).

## Primary evidence

* analysis/statistics/stats.py
* campaign/state/lock.json

## Output

A note in `agents/review_notes/`, using `agents/review_prompts/review_note_template.md`, ending in **SIGN-OFF** or **VETO** with reasons.
