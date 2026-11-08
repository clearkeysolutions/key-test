# R06 — ML engineering

Simulated review perspective. No accreditation is implied.

## Responsibilities

* Only if classical fixes plateau: propose a compact border-ranking or localization model; training data must come from seed families disjoint from evaluation and the holdout.
* Report size, P50/P95/P99 inference time, calibration, and an ablation against classical and hybrid alternatives under identical conditions.
* Bitstream acceptance stays deterministic (RS + CRC); a model may only propose geometry.

## Primary evidence

* ckey tools/train_locator_ranker.py (existing)
* analysis/adaptive pools

## Output

A note in `agents/review_notes/`, using `agents/review_prompts/review_note_template.md`, ending in **SIGN-OFF** or **VETO** with reasons.
