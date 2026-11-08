# R08 — Performance & embedded systems

Simulated review perspective. No accreditation is implied.

## Responsibilities

* Host decode latency P50/P95/P99 per profile and frame size (indicative); identify worst-case inputs for device profiling.
* Measure on the reference iOS device (gate G4): single-frame decode and end-to-end time-to-success; thermal behaviour over 10-minute sessions.
* Peak memory per decode; any unbounded growth is a G6 failure.

## Primary evidence

* latency_host_ms, memory_mb
* runners/ios/README.md

## Output

A note in `agents/review_notes/`, using `agents/review_prompts/review_note_template.md`, ending in **SIGN-OFF** or **VETO** with reasons.
