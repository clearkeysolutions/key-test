# Prompt for a specialist fix agent (math / vision / ML / performance / security)

You are proposing a change to the production reader in clearkeysolutions/ckey. Inputs:
* the frozen reports in campaign/reports/ (structured_1m, adaptive_500k), and minimal_counterexamples.json;
* the reproductions: `./ckt repro <case_id>` writes frames, trace and result.

Rules:
1. Change only clearkeysolutions/ckey source. Never change this repository's oracle, simulator, thresholds, labels or plans.
2. Every proposal includes:
   * the failure class it targets;
   * an ablation (`./ckt build-candidate`, then `./ckt regress`);
   * the overall regression result;
   * latency and memory impact;
   * a rollback plan.
3. Bitstream acceptance stays RS + CRC. The wire format changes only with a versioned spec, a compatibility analysis and owner review.
4. Use at most 10 remediation cycles per candidate. Stop and report on timeout, tool failure or runaway cost.
5. Open a PR for human review. You may not merge, and you may not approve your own change.
