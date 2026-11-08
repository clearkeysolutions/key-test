"""Failure triage: categorized minimal counterexamples and proposed conformance vectors.

* catalogue(): for every (stratum, profile, failure_reason) cell, the mildest
  failing valid cases (lowest severity bin, then highest ppm), with the
  reproduction command, retained image (if any) and trace URI.
* conformance_vectors(): proposed Class B/C vectors (case lines + expected
  payload hash) for a reviewed PR to clearkeysolutions/ckey tests/vectors/.
"""
from __future__ import annotations

import json
import sqlite3
from pathlib import Path


def catalogue(con: sqlite3.Connection, cohort: str = "structured", per_cell: int = 5) -> dict:
    cells = con.execute("""SELECT stratum, variant, failure_reason, COUNT(*) FROM cases
                           WHERE cohort=? AND success=0 AND neg_kind='none' AND phys_class!='UNRECOVERABLE'
                           GROUP BY stratum, variant, failure_reason ORDER BY COUNT(*) DESC""", (cohort,)).fetchall()
    out = {}
    for s, p, reason, n in cells:
        rows = con.execute("""SELECT case_id, sev_bin, severity, ppm_min, gt_quad_decodes, localization_error_modules, image_uri, trace_uri, transformations
                              FROM cases WHERE cohort=? AND stratum=? AND variant=? AND failure_reason=? AND success=0 AND neg_kind='none'
                              AND phys_class!='UNRECOVERABLE' ORDER BY sev_bin ASC, ppm_min DESC LIMIT ?""", (cohort, s, p, reason, per_cell)).fetchall()
        out[f"{s}/{p}/{reason}"] = {"count": n, "examples": [
            {"case_id": r[0], "sev_bin": r[1], "severity": r[2], "ppm_min": r[3], "gt_quad_decodes": r[4], "loc_err_modules": r[5],
             "image": r[6] or None, "trace": r[7], "conditions": r[8], "repro": f"./ckt repro {r[0]}"} for r in rows]}
    wrong = con.execute("SELECT case_id, stratum, profile, neg_kind, image_uri, trace_uri FROM cases WHERE wrong_payload=1").fetchall()
    return {"cohort": cohort, "cells": out, "wrong_payloads": [dict(zip(("case_id", "stratum", "profile", "neg_kind", "image", "trace"), w)) for w in wrong]}


def conformance_vectors(con: sqlite3.Connection, data_dir: Path, limit_per_class: int = 50) -> dict:
    from generator import plan as planmod  # local import: plan reading is only needed here
    want = {}
    q_b = """SELECT case_id, plan_set, payload_hash FROM cases WHERE cohort='structured' AND stratum='S01' AND neg_kind='none' AND success=1
             AND ideal=1 ORDER BY ppm_min ASC LIMIT ?"""
    q_c = """SELECT case_id, plan_set, payload_hash FROM cases WHERE cohort='structured' AND neg_kind='none' AND success=0 AND phys_class='RECOVERABLE'
             ORDER BY sev_bin ASC, ppm_min DESC LIMIT ?"""
    for cls, q in (("B_static_decoder", q_b), ("C_live_reader_counterexample", q_c)):
        for cid, ps, ph in con.execute(q, (limit_per_class,)):
            want[cid] = (cls, ps, ph)
    vectors = {"B_static_decoder": [], "C_live_reader_counterexample": []}
    by_set: dict = {}
    for cid, (cls, ps, ph) in want.items():
        by_set.setdefault(ps, {})[cid] = (cls, ph)
    for ps, ids in by_set.items():
        for c in planmod.iter_plan(data_dir, ps):
            if c["case_id"] in ids:
                cls, ph = ids[c["case_id"]]
                vectors[cls].append({"case": c, "expected_payload_sha256": ph,
                                     "note": "regenerate frames with ckt_worker --repro; expected outcome is an exact decode"})
    return {"status": "PROPOSED (requires review and a PR to clearkeysolutions/ckey)", "vectors": vectors}
