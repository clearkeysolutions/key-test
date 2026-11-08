# CKey reader hardening campaign (CKTEST-RH-001)

This is the temporary execution repository for the mandate in [SPEC.md](SPEC.md). It contains a deterministic harness that tests the production CKey encoder, renderer and full-frame reader from [`clearkeysolutions/ckey`](https://github.com/clearkeysolutions/ckey), pinned to one commit. It runs them through a staged camera simulation, records every case, and covers:

* the 1,000,000 structured cases;
* the 500,000 adaptive cases;
* the sealed 100,000-case holdout;
* gate evaluation;
* archival.

What this repository does **not** do:
* claim certification;
* ship a fork of the reader;
* delete anything.

Baseline facts, discrepancies and open owner decisions are in [CONTEXT.md](CONTEXT.md).

## Running the whole campaign on a dedicated machine

You need Ubuntu 22.04/24.04 (or similar) with 8+ cores, 32 GB RAM and about 60 GB free disk.

```bash
sudo apt install -y build-essential cmake ninja-build git python3 python3-venv openssh-client clang libclang-rt-18-dev tmux
```

```bash
git clone https://github.com/clearkeysolutions/key-test && cd key-test && git checkout claude/gifted-edison-fjs8l3
```

```bash
scripts/preflight.sh
```

Preflight does the following:
1. Clones `ckey` at the pinned commit into `third_party/ckey`.
2. Creates `.venv`.
3. Builds the worker (Release, `-ffp-contract=off`).
4. Runs the selftest.
5. Checks config/schema consistency.
6. Benchmarks **this** machine and prints an ETA. On this harness's development container, a 4K case costs about 1.1 CPU-seconds, so plan for roughly 2–3 days of wall time for pilot + 1M + 500k on 8 cores.

Then start the campaign inside tmux so it survives disconnects:

```bash
tmux new -s ckt 'scripts/run.sh --approver "Your Name"; bash'
```

`--approver` records, in a signed lock, that you approve the release gates in `configs/gates.yaml` and the pre-registered design. Review both files first. The run then goes through:
1. `BASELINE_LOCKED`
2. plan generation (pilot, structured, sealed holdout)
3. plan validation
4. the 10,000-case pilot gate
5. the **1,000,000** structured cases
6. classification and quota reconciliation
7. the frozen, signed `REPORT_1M`
8. weakness scoring
9. the **500,000** adaptive cases (350k failure-targeted, 50k cross-condition, 100k boundary search in rounds)
10. the sanitizer and fuzz security pass

It stops at `PROPOSE_FIX`. Fixing the reader is a human-reviewed step.

If anything stops the run (reboot, budget ceiling, `HALT`), continue with:

```bash
scripts/resume.sh
```

Completed cases are never re-run, and results are append-only.

Watch it with:

```bash
./ckt status
```

The live log is in `campaign/logs/`, and reports appear in `campaign/reports/`.

## After the automated stages

| Step | Command |
|---|---|
| Inspect the baseline | `campaign/reports/structured_1m.md`, `adaptive_500k.md`, `minimal_counterexamples.json` |
| Reproduce any case bit-for-bit | `./ckt repro <case_id>` (frames as PNG, trace, hash check) |
| Build a candidate fix | `./ckt build-candidate --ckey /path/to/ckey-with-fix --label fix1` |
| Regression (golden suite, Holm-adjusted non-inferiority) | `./ckt regress --label fix1` |
| Independent holdout (sealed seed) | `./ckt holdout --label fix1` (or `--label baseline`) |
| Gates and label | `./ckt review` |
| Offline verification | `scripts/verify.sh 100` |
| Archive to permanent storage | `scripts/archive.sh /mnt/permanent` |

The highest label the tooling can assign is `CERTIFICATION_CANDIDATE`. `CERTIFIED_TO_CKEY_PROFILE_<version>` requires a signed disposition by the independent certification reviewer; see `certification/qualification/README.md`. Gate G4 (device latency) and the real-device corpus need an iPhone. See `runners/ios/` and `certification/qualification/real_device_protocol.md`.

## Quick checks

| | |
|---|---|
| Smoke run, about 1 minute | `./ckt smoke --cases 60` |
| Whole pipeline at 1/2000 scale (never qualifies) | `CKT_TEST_SCALE=2000 ./ckt --data-dir /tmp/ckt-test run --approver test` |
| Selftest | `./build/native/ckt_worker --selftest` |
| Sanitizers and fuzzing | `scripts/security.sh` (writes `campaign/reports/security.json`) |

## Layout

```
configs/      baseline lock inputs, strata design, gates, adaptive rules, devices, run settings
generator/    deterministic plan generation: seeds, payloads, devices/pose, negatives, the 20 strata
simulation/   staged image formation (render, printer, display, scene, optics, sensor, pipeline)
runners/      native worker + production-reader adapter, sharded supervisor, campaign state machine, iOS plan
analysis/     SQLite index, statistics, reports, adaptive selection, triage
certification/ gates evaluator, conformance & qualification procedures
security/     sanitizer + fuzz runner, libFuzzer targets, artifact signing
agents/       review-board roles, review prompts, approval rules (simulated panel, no accreditation implied)
schemas/      case, result and manifest JSON schemas
scripts/      preflight, run, resume, verify, archive, security
workflows/    CI smoke workflow and campaign workflow notes
reports/      small, committed summaries (large data lives in campaign/, which is not in git)
campaign/     (generated, gitignored) plans, results, traces, images, DB, state, reports, signatures
```

## Guarantees and limits, briefly

* **Real reader only.** Every decode goes through the production `ckey_find_borders_gray8` → `ckey_refine_quad_gray8` → `ckey_decode_*_gray8` loop the iOS app uses, built from the pinned commit. There are no mock decoders.
* **Deterministic.** Every case is a pure function of its plan line. Frames are SHA-256 hashed, and `./ckt repro` regenerates them bit for bit on the same platform.
* **Counting.** A reader crash or hang is a completed failure. A harness fault is rerun, and a repeated harness fault halts the campaign. Invalid generation halts. Nothing is silently skipped.
* **Physical honesty.** Each case gets a physical class: `RECOVERABLE` / `MARGINAL` / `UNRECOVERABLE`. The labels come from measured px/module, MTF × contrast vs noise (d′), occlusion vs ECC budget, and cropping. Impossible cases are reported separately rather than as reader failures.
* **Simulation is not reality.** The camera models are generic until they are validated against a real capture corpus. Server timings are not iPhone timings.
