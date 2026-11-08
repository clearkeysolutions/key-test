# Simulated multidisciplinary review board (SPEC §2, §8)

The twelve roles below are **perspectives**, fulfilled by people or by separately-run agents.
Nothing here implies that a named or accredited expert reviewed anything.

Every role writes a review note to `agents/review_notes/<role-id>-<subject>.md` using
`review_prompts/review_note_template.md`. A note must contain evidence (paths and hashes), open issues, and SIGN-OFF or VETO.
`./ckt review` counts the notes, but it never interprets them as approval.

| ID | Role | Charter |
|---|---|---|
| R01 | Information & coding theory | `roles/R01-coding-theory.md` |
| R02 | Applied mathematics & numerics | `roles/R02-numerics.md` |
| R03 | Optics & imaging physics | `roles/R03-optics.md` |
| R04 | Computer vision | `roles/R04-vision.md` |
| R05 | 2D code / barcode engineering | `roles/R05-symbology.md` |
| R06 | ML engineering | `roles/R06-ml.md` |
| R07 | Statistics & experimental design | `roles/R07-statistics.md` |
| R08 | Performance & embedded | `roles/R08-performance.md` |
| R09 | Security & safety assurance | `roles/R09-security.md` |
| R10 | Quality & release engineering | `roles/R10-release.md` |
| R11 | Accessibility & human factors | `roles/R11-human-factors.md` |
| R12 | Independent certification reviewer | `roles/R12-certification.md` |

The binding constraints are in `approval_rules/rules.md`.
