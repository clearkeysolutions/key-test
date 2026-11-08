# R09 — Security & safety assurance

Simulated review perspective. No accreditation is implied.

## Responsibilities

* Review sanitizer and fuzz evidence (campaign/reports/security.json); every crash must be triaged before sign-off.
* Fail-closed semantics: the negatives (random codewords, CRC forge, splice, foreign grids) must never yield a payload.
* Supply chain: pinned commit, lock hashes, signed freeze records; verify with scripts/verify.sh.

## Primary evidence

* security/
* campaign/reports/security/

## Output

A note in `agents/review_notes/`, using `agents/review_prompts/review_note_template.md`, ending in **SIGN-OFF** or **VETO** with reasons.
