# Workflows

* `.github/workflows/smoke.yml` (CI). On every push it builds against the pinned ckey commit, runs the selftest, and does a 40-case smoke run. It deliberately does not run campaign volumes.
* **Campaign workflow (dedicated machine).** `scripts/preflight.sh`, then `scripts/run.sh --approver NAME`, then `scripts/resume.sh` as needed. The state machine (`runners/campaign.py`) enforces the order:
  BASELINE_LOCKED → GENERATE → VALIDATE_ARTIFACT (incl. pilot) → EXECUTE → CLASSIFY → REPORT_1M → SELECT_WEAKNESSES → EXECUTE_500K → PROPOSE_FIX → BUILD → REGRESSION → INDEPENDENT_HOLDOUT → CERTIFICATION_REVIEW → ARCHIVE.
  Every transition is appended to `campaign/state/transitions.jsonl`.
* **Fix loop (human-reviewed).** A specialist proposes a ckey PR (`agents/review_prompts/specialist_fix_prompt.md`). Then:
  1. `./ckt build-candidate --ckey PATH --label L`
  2. `./ckt regress --label L` (golden suite, Holm-adjusted non-inferiority)
  3. the reviewer integrity check (`agents/review_prompts/oracle_integrity_prompt.md`)
  4. `./ckt holdout --label L`
  5. `./ckt review`

  Stop after at most 10 remediation cycles per candidate. A merge needs a human.
* **Archive.** Run `scripts/archive.sh DEST`, then verify `SHA256SUMS` at the destination. Deleting the temporary repository needs explicit owner approval, given immediately beforehand, and is never automated.
