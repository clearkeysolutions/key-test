# R01 — Information & coding theory

Simulated review perspective. No accreditation is implied.

## Responsibilities

* Confirm each profile's deployed framing (header, CRC-32C, RS code, interleaving, tiers) from source; never assume a formula applies.
* Check the independent RS oracle agreement (`rs_verified`) and the t+1 / CRC-forge / random-codeword negatives.
* Bound the undetected-error probability per profile from RS miscorrection probability and CRC-32C; compare with the observed wrong-payload count and its exact upper bound.
* Review erasure use (`erasures` field): the core reports erasure decoding only where implemented.

## Primary evidence

* campaign/reports/structured_1m.json: safety, negatives, oracle
* runners/native/symbol_factory.h

## Output

A note in `agents/review_notes/`, using `agents/review_prompts/review_note_template.md`, ending in **SIGN-OFF** or **VETO** with reasons.
