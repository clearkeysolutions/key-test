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

## Scope (owner decisions, 2026-11-08)

* **Profile 0 only.** The campaign tests P0 (Format 0.27) and P0.1 (Format 0.28 print layout), 25,000 cases each per stratum. Every supported (version 1–31, ECC level) combination is cycled, so all densities are covered. P1/P2 are scheduled for deprecation and P3 needs no tests. The reader adapter still runs the full production loop, so a P0 symbol misread as another profile would still be caught.
* **1080p-class capture, no 4K.** Cameras are 1080p (wide, tele, budget, ultrawide) and 720p. A case first lowers its px/module (moves the camera back) until the symbol fits. The densest versions cannot fit at the declared 6 px/module in 1,080 px; they are imaged below it and flagged `below_declared_min_to_fit`. That measures the reader's real resolution floor.
* **Storage:** big artifacts go to Cloudflare R2 (bucket `ckey-hardening-test`), content-addressed, about 3 objects per 1,000 cases. Small permanent evidence goes into git under `evidence/`: hashes, manifests, reports, counterexample case lines, and gzip -9 SVGs. Frames are never stored per case. Every frame regenerates bit for bit from its case line.
* **ML:** `./ckt ml` builds a separate training cohort and trains a corner-localization MLP (numpy). It then runs a classical vs ML vs ML→refine ablation on identical frames and stores the model and dataset. Payloads are still accepted only by RS + CRC.

## Running on a rented server (from your laptop)

The server needs these as root once (yours already has most of them):

```bash
apt install -y git build-essential cmake ninja-build clang libclang-rt-dev python3 python3-venv tmux openssh-client
```

On your laptop, in a clone of this repo on branch `claude/gifted-edison-fjs8l3`, put your R2 token in place (the file is gitignored):

```bash
cp scripts/server/r2.env.example scripts/server/r2.env && $EDITOR scripts/server/r2.env
```

Ship the commit and start. This gives the server no GitHub credentials, and the campaign runs in tmux:

```bash
scripts/server/send.sh ckeyrunner@62.238.99.101 "Your Name"
```

Watch it:

```bash
scripts/server/status.sh ckeyrunner@62.238.99.101
```

Before the rental ends, push everything to R2 and build an archive bundle:

```bash
ssh ckeyrunner@62.238.99.101 'cd key-test && scripts/server/finish.sh'
```

Then bring the small evidence back into git:

```bash
scripts/server/fetch.sh ckeyrunner@62.238.99.101
```

The `"Your Name"` argument records, in the signed lock, that you approve `configs/gates.yaml` and the design. On the server, `scripts/server/start.sh` runs:
1. preflight (pinned ckey, venv, build, selftest, benchmark/ETA)
2. lock
3. plans
4. pilot
5. the 1,000,000 structured cases
6. the frozen REPORT_1M
7. the 500,000 adaptive cases
8. ML
9. triage
10. sanitizers and fuzzing
11. R2 sync
12. evidence export

It resumes by itself if interrupted; re-run it if the box reboots. At about 0.5 core-seconds per 1080p case, the 1.5M cases take roughly a day on 8 cores. Preflight prints the measured ETA.

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
* **Storage cost.** R2 holds about 3–6 GB in about 5,000 objects for the whole campaign. That is a few cents per month and no egress fees.
