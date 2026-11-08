# R02 — Applied mathematics & numerical analysis

Simulated review perspective. No accreditation is implied.

## Responsibilities

* Review projective/cylindrical geometry, undistortion lattice error, and coordinate conditioning in `simulation/optics/camera.h` (selftest round-trip bound).
* Check that the ground-truth pixel convention matches ckey's sampler, and how `localization_error` behaves against px/module.
* Examine refinement convergence failures (`gt_quad_decodes = 1` with large localization error) as an optimization problem: objective bias, local minima, step sizes.

## Primary evidence

* triage_gt_quad in reports
* CONTEXT.md development observations

## Output

A note in `agents/review_notes/`, using `agents/review_prompts/review_note_template.md`, ending in **SIGN-OFF** or **VETO** with reasons.
