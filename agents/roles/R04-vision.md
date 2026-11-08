# R04 — Computer vision

Simulated review perspective. No accreditation is implied.

## Responsibilities

* Analyse BORDER_MISS / REFINE_REJECTED / UNCORRECTABLE-with-good-GT-quad failures by stratum.
* Review distractor and false-detection behaviour (S16, negatives), split-finder orientation (S05) and corner localization (S19).
* Propose geometry fixes with ablations: multi-scale border search, sub-pixel corner/arc fits, robust homography residuals.

## Primary evidence

* failure_mechanisms and triage tables
* minimal_counterexamples.json

## Output

A note in `agents/review_notes/`, using `agents/review_prompts/review_note_template.md`, ending in **SIGN-OFF** or **VETO** with reasons.
