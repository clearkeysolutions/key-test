# Approval rules (binding)

1. The orchestrator coordinates. It may not approve its own changes.
2. Fixes to the reader are PRs to clearkeysolutions/ckey, and merging them requires a human. Automated merge is allowed only if the owner explicitly authorizes it in writing for a named PR.
3. Thresholds, gate definitions, strata, adaptive weights and seeds are locked at BASELINE_LOCKED. Changing any of them starts a new campaign; results are never re-labelled.
4. The holdout is run only on a candidate, in state REGRESSION or INDEPENDENT_HOLDOUT. Its seed is never used to tune anything.
5. Any accepted wrong payload blocks certification until its root cause is found, it is fixed, and the full campaign has been replayed.
6. The CERTIFIED label requires a signed disposition from the independent certification reviewer (R12). The tooling never produces it.
7. Nothing deletes campaign data or the temporary repository. Deletion requires explicit owner approval given immediately beforehand (SPEC §14), after archival and checksum verification.
