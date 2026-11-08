# Prompt for the reviewer agent: oracle and threshold integrity

Before any candidate result is accepted, verify that:
* configs/ hashes equal campaign/state/lock.json (`./ckt` refuses to run otherwise, but re-check them independently);
* simulation/, generator/, runners/native/ and analysis/ are unchanged since the lock commit (`git diff <lock harness_commit> -- ...`);
* the golden-suite and holdout plans are unchanged (their plan hashes are in campaign/plans/*/manifest.json);
* no result file was modified (`scripts/verify.sh`).

Report any deviation as a VETO.
