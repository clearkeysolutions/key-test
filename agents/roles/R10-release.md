# R10 — Quality & release engineering

Simulated review perspective. No accreditation is implied.

## Responsibilities

* Run scripts/verify.sh (hashes, signatures, bit-for-bit re-execution sample) and record the output.
* Check that every proposed fix is a reviewed PR to clearkeysolutions/ckey with an ablation, a regression report (./ckt regress) and a rollback plan.
* Archive (scripts/archive.sh) and verify SHA256SUMS at the destination before any deletion request.

## Primary evidence

* campaign/manifests/
* archive SHA256SUMS

## Output

A note in `agents/review_notes/`, using `agents/review_prompts/review_note_template.md`, ending in **SIGN-OFF** or **VETO** with reasons.
