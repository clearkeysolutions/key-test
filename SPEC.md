# CKey Reader Hardening and Certification Program

Specification: **CKTEST-RH-001** · Revision 1.0 · Status: proposed engineering mandate · Date: 2026-11-07
Execution repository: `clearkeysolutions/ckey-test` (temporary; implemented in `clearkeysolutions/key-test`, see CONTEXT.md)
Production repository: `clearkeysolutions/ckey` (permanent)

> This document specifies the perspectives and review gates of a *simulated multidisciplinary engineering panel*; it does not claim actual named experts reviewed the design. Numerical acceptance targets below are proposed engineering requirements, not observed performance.

The mandate is reproduced as received. Where this repository implements a section, the implementing path is given in *[brackets]*.

## 1. Mission and non-negotiable outcomes

Design, implement, execute, and independently verify a fully automated reader-hardening campaign of at least 1,500,000 completed test cases against the real production CKey encoder, renderer, and scanner. Exactly 1,000,000 initial structured cases must cover pre-registered conditions in balanced strata. At least 500,000 additional adaptive cases must be allocated using failures and weakness scores observed in the first million. Failures must drive algorithmic, geometric, numerical, parameter, and/or ML improvements, followed by replay and independent validation. Do not substitute generated mock decoders for the real reader. Do not claim certification merely because the test suite completed.

**Definition of completed case:** a uniquely identified case whose input artifact, ground truth, test metadata, execution status, scanner output, timing, and diagnostic trace have been durably recorded. A crash, timeout, or no-read is a completed failed case; a missing artifact, infrastructure failure, or lost result is not completed and must be rerun. Every case is counted once in its designated cohort, regardless of retries; retries are separately recorded. *[runners/sharding/supervisor.py]*

**Top-level invariants:** (a) zero silently accepted wrong payloads is a release gate, not proof of impossibility; (b) a reader may decline to decode when information is insufficient; (c) no unverified ML output bypasses integrity checks; (d) scanning must work offline; (e) all changes to the wire format require versioned specification and compatibility review; (f) no test outcome may be erased by rerunning; (g) the temporary repository must not be deleted before preservation and sign-off.

## 2. Expert review board: independent responsibilities

1. Information theory and coding theory — codeword capacity, Reed–Solomon or actual FEC parameters, interleaving, erasure budgets, minimum distance, payload checksums, decoding failure probability; confirm the deployed format before assuming any formula applies.
2. Applied mathematics and numerical analysis — polar transforms, projective geometry, sampling grids, coordinate conditioning, error propagation, uncertainty and subpixel interpolation.
3. Optics and imaging physics — diffraction, depth of field, MTF, rolling shutter, exposure, shot/read noise, motion blur, printer dot gain, halftoning, ink bleed, screen moiré and display-camera aliasing.
4. Computer vision — finder localization, split-corner orientation, robust homography, radial/ring/grid segmentation, multi-hypothesis tracking, real-time frame fusion and rejection of false detections.
5. 2D optical code and barcode engineering — comparative test methodology, printability, quiet/safe zones, size-dependent capacity, symbol integrity, profile interoperability and standards conformance.
6. ML engineering — architecture, data provenance, augmentation validity, held-out validation, quantization, inference time, drift, calibration, deployment, adversarial robustness and training/evaluation isolation.
7. Statistics and experimental design — preregistered strata, sample allocation, binomial uncertainty, confidence intervals, multiplicity, selection bias, adaptive-sampling inference and stopping rules.
8. Performance and embedded systems — portable C/C++ performance, SIMD/NEON, memory budgets, threading, energy, frame latency, iOS camera pipeline, thermal throttling.
9. Security and safety assurance — parser fuzzing, memory sanitizers, integrity verification, supply-chain controls, signed artifacts, denial-of-service, malicious visual inputs, fail-closed semantics.
10. Quality and release engineering — hermetic builds, reproducible seeds, CI orchestration, failure minimization, artifact manifests, immutable audit trail, rollback and release qualification.
11. Accessibility and human factors — camera feedback, low-light guidance, shaky hands, accessible audio/haptics, user-visible false-success prevention.
12. Independent certification reviewer — owns holdout and final release decision, cannot alter thresholds after results, must veto unsupported certification claims.

Every role must produce an explicit review note with evidence, open issues, and sign-off or veto. Automated agents may fulfill these roles but must not imply real professional accreditation. *[agents/]*

## 3. Baseline and repository boundaries

Before testing, inventory the clearkeysolutions/ckey source, supported profiles P0/P1/P2/P3, current spec version, encoder/renderer/decoder ABI, iOS camera behavior, CLI commands, golden vectors, and performance baselines. Preserve the existing CKey geometry: outer grid matrix; ring matrix with uniform small dots and multi-unit arcs; narrow radial ray matrix with variable ray lengths/weights; corner finder arcs inside the border, top-left split for orientation; protected finder exclusion zones; center clear area; no grid under finders. Treat user-established profiles as intent until reconciled with current canonical format. Record any discrepancy; do not silently change geometry or capacity. *[CONTEXT.md]*

The temporary repository contains harnesses, simulators, runner infrastructure, synthetic assets, reports, and test-specific agent orchestration. Production fixes must be proposed and merged into clearkeysolutions/ckey via reviewed pull requests. Do not fork an independent divergent production reader into ckey-test. Use immutable commit SHA, compiler/toolchain lockfile, runner image digest, model digest, and spec hash for each run. *[configs/baseline.yaml, campaign/state/lock.json]*

## 4. Structured campaign: exactly 1,000,000 cases

Use 20 primary strata × 50,000 completed cases each. Within each stratum, distribute profiles P1/P2/P3/P0 as evenly as possible (12,500 each), subject to valid payload generation. Further balance payload lengths, bit densities, codeword patterns, symbol sizes, seeds, devices/sensor models, and print/screen substrates. Use controlled single-factor sweeps for at least 40% of each stratum and randomized valid joint conditions for the remainder. The baseline stratum is clean and must be included in the million. *[configs/strata.yaml, generator/]*

| ID | Primary stratum | Cases |
|---|---|---:|
| S01 | Clean, ideal reference and exact round-trip | 50,000 |
| S02 | Payload length, density, pathological bit patterns | 50,000 |
| S03 | Symbol scale, downsampling, subpixel phase | 50,000 |
| S04 | Perspective, skew, nonplanar warping | 50,000 |
| S05 | Rotation, mirror/reflection rejection, finder orientation | 50,000 |
| S06 | Gaussian/optical defocus and lens PSF | 50,000 |
| S07 | Directional and rolling-shutter motion blur | 50,000 |
| S08 | Low light, photon/read noise, high ISO | 50,000 |
| S09 | Contrast, uneven illumination, shadows | 50,000 |
| S10 | Specular glare, saturation, highlights | 50,000 |
| S11 | Occlusion, tears, scratches, missing regions | 50,000 |
| S12 | Print dot gain, ink bleed, halftone, paper | 50,000 |
| S13 | Screen pixels, RGB subpixels, moiré, refresh artifacts | 50,000 |
| S14 | Compression, sharpening, denoising, ISP artifacts | 50,000 |
| S15 | Color foreground/background, gamma, transparency | 50,000 |
| S16 | Clutter, distractors, partial crops, multiple codes | 50,000 |
| S17 | Ring matrix dot/arc and radial sampling stress | 50,000 |
| S18 | Ray matrix length/weight, angular aliasing | 50,000 |
| S19 | Grid matrix, finder-safe-zone and corner localization | 50,000 |
| S20 | Combined realistic conditions and temporal frame sequences | 50,000 |
| **Total** | | **1,000,000** |

**Coverage control:** define fixed bins for severity and minimum effective pixels per smallest module, angle, blur, occlusion, luminance, code density, and profile. A synthetic image must have physically plausible optics and sensor response; label physically impossible or unrecoverable cases rather than counting them as ordinary failures. Pair valid and deliberately invalid symbols to test rejection. At least 10% of each applicable stratum must include hard negatives or near-miss patterns; these remain within the 50,000 quota and must be reported separately. Temporal sequences count as one test case per independently tracked scan attempt, not per frame; record every frame.

## 5. Camera and physical simulation

Use a staged image formation model: ideal vector symbol → substrate/print/display response → 3D scene projection → lens PSF and distortion → exposure/integration and rolling shutter → sensor mosaic/shot/read noise → ISP demosaic/denoise/sharpen/tone map → compression/rescale → scanner frame. Use linear-light operations where appropriate. Include deterministic pseudorandom seeds and ground-truth module geometry. Validate synthetic distortion distributions against a small real capture corpus before asserting realism. Parameterize physical distances in camera/sensor units when known; do not equate a blur sigma in output pixels to a universal physical severity.

Use a full camera model for screen-to-camera moiré and print imaging, not solely Gaussian blur and arbitrary pixel noise. Maintain both mathematically perfect and physically modeled images. Include at least two independent rendering implementations or vector rasterization cross-checks for oracle independence. Record intrinsic/extrinsic camera parameters, exposure, noise levels, module size, substrate properties, transformation matrices, and occlusion masks. *[simulation/]*

## 6. Scanner hardening mechanisms

Implement changes only when backed by reproducible failures and measured improvements:

* **Geometry:** multi-scale finder detection; subpixel corner/arc localization; split-finder orientation verification; robust homography with residual diagnostics; local nonlinear correction for nonplanar symbols; polar coordinate ring/ray sampling; safe-zone masking.
* **Sampling:** oversampled module measurements; local adaptive thresholding; linear-light luminance; dot/arc shape likelihoods; explicit confidence and erasure labels; reject unsampled or ambiguous modules rather than hallucinating bits.
* **Decoding:** verify exact profile framing and error-correction math; soft-input or reliability-ordered candidates only where supported; bound alternative hypotheses and time; validate length, profile, integrity and FEC syndromes; require full payload consistency before success.
* **Video:** single-frame fast path; temporal tracking keyed to a specific physical symbol; align observations before confidence fusion; avoid mixing distinct codes; reset after tracking uncertainty; prevent stale scan replays.
* **Performance:** incremental region-of-interest tracking; adaptive resolution; NEON/SIMD optimization; bounded allocations; deterministic latency budget; never trade correctness for apparent speed.
* **ML option:** train a compact finder/localization or confidence model only if classical methods plateau on the held-out failure classes; compare classical, ML, and hybrid approaches under identical conditions. Keep final bitstream validation deterministic. Report model size, inference P50/P95/P99, energy, device compatibility and calibration. OTA updates may deliver approved data/model assets, not arbitrary native executable code.

Every proposed change must include an ablation study, failure-class improvement, overall regression check, latency/memory impact, and rollback mechanism. Prefer the simplest robust solution; no ML requirement is imposed if deterministic methods win.

## 7. Adaptive campaign: minimum 500,000 additional cases

Freeze and publish results of the first 1,000,000 before selecting adaptive cases. Compute weakness scores per profile × primary stratum × severity bin × failure mechanism. Example score: weighted combination of no-read rate, accepted-wrong-payload incidence, P95 latency excess, geometric residuals, and uncertainty-bound width; report exact weights before adaptation. Safety-critical wrong-payload cases get priority regardless of frequency. *[configs/adaptive.yaml, analysis/adaptive/]*

Allocate 500,000 cases as follows:

* **350,000 failure-targeted:** weighted allocation to the worst-performing bins; require at least 5,000 per selected bin and cap any one bin at 20% of the adaptive pool unless an independently documented safety-critical exception is approved.
* **100,000 boundary-search:** search failure/success transitions by binary search, Bayesian optimization or adversarial perturbations; minimize failing examples and test nearby perturbations.
* **50,000 cross-condition validation:** systematically recombine the top failure modes with distinct profiles, payloads, scales and device models to detect interactions and overfitting.

These 500,000 must be executed even if the first million has no failures; then target narrowest success margins, highest latency, least confidence and physically challenging boundaries. Maintain a separate untouched holdout validation set (recommended 100,000+ additional cases) and a real-device corpus; holdouts do not count toward the mandatory 1.5 million. Adaptive results must not be pooled naively with the structured million to estimate real-world success prevalence; report conditional/adaptive and stratified baseline estimates separately.

## 8. Automation, agent roles and feedback loop

Implement a durable state machine: BASELINE_LOCKED → GENERATE → VALIDATE_ARTIFACT → EXECUTE → CLASSIFY → REPORT_1M → SELECT_WEAKNESSES → EXECUTE_500K → PROPOSE_FIX → BUILD → REGRESSION → INDEPENDENT_HOLDOUT → CERTIFICATION_REVIEW → ARCHIVE. *[runners/campaign.py]*

An orchestration agent coordinates but may not self-approve its changes. Specialist agents independently propose mathematical, vision, ML, performance and security fixes. A separate evaluator reruns the unchanged golden suite and locked holdout on every candidate. A reviewer agent validates that no oracle, acceptance threshold, or test label was changed to improve scores. A release agent creates PRs in the production repo; merge requires human approval unless explicitly authorized otherwise.

Use bounded remediation cycles (e.g. up to 10 per candidate), CI concurrency limits, resumable sharded jobs, checkpointing, budget ceilings, idempotent case IDs, and immutable manifests. On timeout, tool failure, runaway cost, invalid test generation or model collapse, stop and report; do not silently skip cases. Keep signed machine-readable artifacts and human-readable summaries. Never assume GitHub-hosted runners can economically execute 1.5 million full camera sequences; support local machines or scalable dedicated runners and preflight resource estimates.

## 9. Data isolation and statistical integrity

Split by payload family, seed family, rendering pipeline, device model, and acquisition session, not just random image file, to prevent leakage. Lock all evaluation seeds and holdouts before training. Never use adaptive test labels as an independent unbiased estimate of field performance. Define operational use-case weights separately from equal stratum allocation. Report Wilson or exact binomial confidence intervals for independent Bernoulli cohorts, with clustering corrections for correlated frames/payload families. For zero wrong decodes in N independent trials, the approximate one-sided 95% upper rate bound is 3/N; at N=1,500,000 it is ~2 per million, not zero. Correlation makes this bound weaker. Report per-profile and per-severity uncertainty, not only aggregate accuracy. *[analysis/statistics/]*

## 10. Metrics and release gates

Required per-case fields: case_id, cohort, stratum, profile, payload_hash, format_version, seed, renderer_sha, scanner_sha, model_sha, camera_model, transformations, image_hash, expected_outcome, observed_outcome, wrong_payload, finder_detected, orientation_correct, localization_error, FEC_corrections, erasures, frame_count, elapsed_ms, peak_memory, failure_reason, trace_uri. *[schemas/result.json]*

Proposed release criteria (must be approved before execution): *[configs/gates.yaml]*

1. No accepted wrong payload in the entire qualification campaign and independent holdout. Any occurrence blocks certification until root cause, fix and complete replay.
2. 100% exact round-trip for clean valid symbols that satisfy the canonical spec and declared minimum module resolution.
3. ≥99.9% scan success on the separately defined and frozen normal-use distribution, with confidence bounds disclosed; do not demand successful recovery of physically unrecoverable inputs.
4. P95 ≤200 ms on an explicitly defined reference iOS device and reference single-frame decode workload; measure camera acquisition separately and report end-to-end time-to-success distributions.
5. No statistically or practically significant regression in any supported profile or important severity bin versus baseline; define effect-size margins in advance.
6. Zero sanitizer-detected memory safety defects, critical parser crashes, unbounded memory growth, or unbounded adversarial compute on qualified builds.
7. Complete coverage evidence: exactly 1,000,000 structured plus at least 500,000 adaptive completed cases, all with verifiable manifests; independent holdout and real-device validations complete.

Release labels: UNQUALIFIED, SYNTHETICALLY_HARDENED, DEVICE_VALIDATED, CERTIFICATION_CANDIDATE, CERTIFIED_TO_CKEY_PROFILE_<version>. Certification is internal CKey conformance certification, not a third-party accreditation or regulatory certification. Publish scope, tested devices, profile versions, limits, confidence intervals, exclusions and revocation rules. Do not use CERTIFIED unless an independent authorized reviewer approves the evidence.

## 11. Certification hardening and standard changes

Generate a Certification Hardening Report that maps failures to normative spec clauses, proposed conformance vectors, minimum print/display module size, finder exclusion-zone dimensions, ray/ring sampling tolerances, symbol contrast requirements, FEC integrity and minimum camera test conditions. Separate: (A) renderer conformance, (B) static decoder conformance, (C) live camera reader qualification, (D) cross-platform interoperability. Do not revise the symbol format merely to fit a deficient scanner; require mathematical proof and compatibility analysis before format changes. For every proposed certification threshold, include physical justification, observed margin, confidence interval, backward compatibility, and versioning impact. Freeze certification vectors and maintain them permanently in clearkeysolutions/ckey. *[certification/]*

## 12. Temporary repository structure

```
ckey-test/
  README.md  SPEC.md  CONTEXT.md
  configs/strata.yaml  configs/gates.yaml
  generator/{payloads,geometry,negative_cases}/
  simulation/{optics,sensor,printer,display,video}/
  runners/{native,ios,sharding}/
  analysis/{metrics,statistics,adaptive,triage}/
  agents/{roles,review_prompts,approval_rules}/
  security/{fuzz,sanitizers,artifact_signing}/
  certification/{conformance,qualification}/
  schemas/{case,result,manifest}.json
  workflows/  reports/
  scripts/{preflight,run,resume,verify,archive}.sh
```

Version all input schemas, simulator physics assumptions, deterministic PRNG and seed derivation, and scanner API adapter. Include reproducible one-command local smoke run, one-command resumed campaign, and offline artifact verification. Large generated assets should live in content-addressed object storage with checksums, not unbounded Git history.

## 13. Deliverables and acceptance evidence

(1) running reproducible 1.5M+ campaign; (2) immutable results database and manifests; (3) baseline and adaptive reports; (4) categorized minimal counterexamples; (5) merged reader improvements with ablation results; (6) frozen independent holdout results; (7) iOS real-camera field validation; (8) hardened CKey certification suite and versioned rules; (9) regression vectors moved into production repository; (10) final multidisciplinary review and signed disposition; (11) documented known limitations and future work.

## 14. Repository archival and deletion policy

clearkeysolutions/ckey-test is intentionally disposable, but deletion is prohibited until all essential tests, conformance vectors, minimal reproducers, simulator source, metrics, statistical scripts, signed manifests, certification decisions, and production fixes have been copied to permanent versioned storage; checksum-verified; and independently reproducible. Preserve enough data to audit claims without the temporary repository. Large raw images may be selectively retained by documented policy, but preserve seeds, simulation versions, and representative failure images. Obtain explicit owner approval immediately before deleting the repo. Do not automate destructive deletion as part of CI.

## 15. Execution plan and exit checklist

* Phase 0: inventory real repo and freeze baseline; verify symbol format, current scanner API, licensing and build environment.
* Phase 1: build independent reference vectors, optics simulator, deterministic harness, schema and preflight.
* Phase 2: run pilot (e.g. 10,000) to validate simulator and instrumentation; pilot does not count unless formally locked into the structured plan before execution.
* Phase 3: execute exactly 1,000,000 balanced structured cases; freeze results.
* Phase 4: score weaknesses and execute at least 500,000 targeted cases.
* Phase 5: propose, implement and ablate fixes; replay all relevant tests plus independent holdout.
* Phase 6: test real devices, certify profile-specific limits and release changes through reviewed PRs.
* Phase 7: migrate permanent assets, verify reproducibility and seek explicit approval for temporary repo deletion.

Exit requires: quota reconciliation; zero uninvestigated wrong decodes; transparent failure distribution; independent holdout; validated performance; signed artifacts; profile-specific certification; permanent reproducibility; documented owner sign-off.

**Important boundary:** this specification authorizes building a test system and preparing changes, not asserting that the tests have already run, that experts have personally reviewed results, that a certificate has been earned, or that any repository may be deleted without further authorization.
