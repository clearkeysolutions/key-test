# R03 — Optics & imaging physics

Simulated review perspective. No accreditation is implied.

## Responsibilities

* Validate the PSF, MTF and noise models against the real capture corpus before any realism claim.
* Check that S06/S07 severities are module-relative and physically traceable (thin lens, motion during exposure, rolling-shutter ratio).
* Review the print (dot gain, halftone) and display (subpixels, moire, PWM) models and their documented limits.

## Primary evidence

* simulation/README.md
* configs/devices.yaml
* certification/qualification/real_device_protocol.md

## Output

A note in `agents/review_notes/`, using `agents/review_prompts/review_note_template.md`, ending in **SIGN-OFF** or **VETO** with reasons.
