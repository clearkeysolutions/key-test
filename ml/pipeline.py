"""./ckt ml: dataset -> train -> evaluate -> store (SPEC §6 ML option).

1. Plan set `mltrain`: N valid P0/P0.1 cases across all 20 strata (joint + sweep), own seed.
2. ckt_worker --ml-export per shard (parallel): one 2.4 KB record per production border
   candidate; frames are not stored (regenerable from the case lines).
3. ml/train.py: corner MLP; weights + model card in ml/models/.
4. ckt_worker --ml-eval on (a) held-out mltrain cases and (b) structured failures and
   successes if the structured DB exists: classical vs ML vs hybrid on identical frames,
   wrong payloads, inference time. Report: campaign/reports/ml.{json,md}.
5. Records and model uploaded to R2 (content-addressed) when configured.
The model is never wired into the production reader by this repository; adopting it is a
reviewed PR to clearkeysolutions/ckey with the ablation this step produces.
"""
from __future__ import annotations

import gzip
import json
import os
import random
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from generator import plan as planmod
from generator.common import PROFILES, ROOT, load_yaml, scaled, worker_path
from generator.strata import STRATA, make_case
from ml import train as trainer

MASTER = "CKTEST-RH-001/mltrain/v1"


def build_plan(data_dir: Path, n: int) -> None:
    if (data_dir / "plans" / "mltrain" / "manifest.json").exists():
        return
    strata = sorted(STRATA)
    cases = []
    for k in range(n):
        s, v, b = strata[k % 20], PROFILES[(k // 20) % len(PROFILES)], (k // 40) % 5
        c = make_case("mltrain", MASTER, s, v, b, "joint" if k % 3 else "sweep", k, case_id=f"mltrain-{s}-{v}-b{b}-{k:06d}")
        c.pop("_notes", None)
        if c.get("second_code"):
            c["second_code"] = 0
            c["expected"] = "DECODE"
        cases.append(c)
    planmod.write_cases(data_dir, "mltrain", cases, {"purpose": "ML training data (not counted)"})


def _run_shards(data_dir: Path, args_for, workers: int) -> None:
    shards = sorted((data_dir / "plans" / "mltrain").glob("shard-*.jsonl.gz"))
    def one(sh: Path):
        args = args_for(sh)
        if args is None:
            return
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
    with ThreadPoolExecutor(max_workers=workers) as ex:
        list(ex.map(one, shards))


def run(c, cases: int | None = None, eval_cases: int | None = None) -> dict:
    cfg = load_yaml("campaign.yaml").get("ml", {})
    n = cases or scaled(cfg.get("train_cases", 40000), 200)
    ne = eval_cases or scaled(cfg.get("eval_cases", 6000), 100)
    workers = os.cpu_count() or 1
    data = c.data
    rec_dir = data / "ml" / "records"
    rec_dir.mkdir(parents=True, exist_ok=True)
    c.log(f"ML: plan set mltrain ({n:,} cases)")
    build_plan(data, n)

    def export_args(sh: Path):
        out = rec_dir / (sh.name.replace(".jsonl.gz", ".bin"))
        done = out.with_suffix(".done")
        if done.exists():
            return None
        out.unlink(missing_ok=True)
        plain = rec_dir / (sh.name.replace(".gz", ""))
        with gzip.open(sh, "rt") as fi:
            plain.write_text(fi.read())
        return ["sh", "-c", f"'{worker_path()}' --ml-export '{plain}' '{out}' && touch '{done}' && rm -f '{plain}'"]
    c.log("ML: exporting candidate records (production border finder on every case)")
    _run_shards(data, export_args, workers)
    files = sorted(rec_dir.glob("shard-*.bin"))
    c.log(f"ML: training on {sum(f.stat().st_size for f in files) / 1e6:.1f} MB of records")
    card = trainer.train(files, ROOT / "ml" / "models", epochs=cfg.get("epochs", 15), log=c.log)
    model = ROOT / "ml" / "models" / card["model"]
    # Evaluation sets.
    import hashlib
    held = [x for x in planmod.iter_plan(data, "mltrain")
            if (int(hashlib.sha256(x["case_id"].encode()).hexdigest()[:4], 16) * 2654435761 % 10) == 0][:ne]
    evals = {"mltrain_heldout": held}
    db = data / "db" / "results.sqlite"
    if db.exists():
        import sqlite3
        con = sqlite3.connect(db)
        ids = [r[0] for r in con.execute("""SELECT case_id FROM cases WHERE cohort='structured' AND neg_kind='none' AND second_code=0
                                            AND phys_class!='UNRECOVERABLE' AND success=0 ORDER BY case_id""")]
        oks = [r[0] for r in con.execute("""SELECT case_id FROM cases WHERE cohort='structured' AND neg_kind='none' AND second_code=0 AND success=1 ORDER BY case_id""")]
        random.Random(11).shuffle(ids)
        random.Random(12).shuffle(oks)
        want = set(ids[:ne] + oks[:ne // 3])
        if want:
            evals["structured_sample"] = [x for x in planmod.iter_plan(data, "structured") if x["case_id"] in want]
    report = {"model_card": card, "evaluations": {}}
    for name, items in evals.items():
        if not items:
            continue
        chunk = max(1, len(items) // workers + 1)
        outs = []
        def ev(i):
            part = items[i:i + chunk]
            p = data / "ml" / f"eval-{name}-{i}.jsonl"
            o = data / "ml" / f"eval-{name}-{i}.out.jsonl"
            o.unlink(missing_ok=True)
            p.write_text("".join(json.dumps(x) + "\n" for x in part))
            subprocess.run([str(worker_path()), "--ml-eval", str(p), str(model), str(o)], check=True)
            return o
        with ThreadPoolExecutor(max_workers=workers) as ex:
            outs = list(ex.map(ev, range(0, len(items), chunk)))
        rows = [json.loads(l) for o in outs for l in o.read_text().splitlines() if l.strip()]
        n_ = len(rows)
        summ = {"cases": n_}
        for k in ("ok_classical", "ok_ml", "ok_ml_refine", "ok_hybrid", "wrong_classical", "wrong_ml", "wrong_ml_refine"):
            summ[k] = sum(1 for r in rows if r[k])
        ms = sorted(r["ml_ms"] for r in rows)
        summ["ml_inference_ms_p50"] = ms[len(ms) // 2] if ms else None
        summ["ml_inference_ms_p95"] = ms[int(.95 * (len(ms) - 1))] if ms else None
        rescued = [r for r in rows if (r["ok_ml"] or r["ok_ml_refine"]) and not r["ok_classical"]]
        lost = [r for r in rows if r["ok_classical"] and not r["ok_ml"]]
        summ["rescued_by_ml"] = len(rescued)
        summ["classical_only"] = len(lost)
        by = {}
        for r in rows:
            k = r["stratum"]
            d = by.setdefault(k, {"n": 0, "classical": 0, "hybrid": 0})
            d["n"] += 1; d["classical"] += r["ok_classical"]; d["hybrid"] += r["ok_hybrid"]
        summ["by_stratum"] = by
        report["evaluations"][name] = summ
    (data / "reports").mkdir(parents=True, exist_ok=True)
    (data / "reports" / "ml.json").write_text(json.dumps(report, indent=1))
    lines = ["# ML localization option: dataset, model, ablation", "",
             "> Geometry proposals only: every payload is still accepted solely by the production RS + CRC checks. "
             "Adoption in the reader is a reviewed PR to clearkeysolutions/ckey.", "",
             f"Model `{card['model']}` ({card['parameters']:,} parameters, {card['bytes'] / 1024:.0f} KB), {card['architecture']}.", "",
             f"Validation corner error (side units): ML median {card['val_corner_error_side_units']['ml_median']:.4f}, "
             f"classical refine median {card['val_corner_error_side_units']['classical_refine_median']}", "",
             "| Evaluation | Cases | Classical | ML quad | ML -> refine | Hybrid | Rescued by ML | Wrong (classical / ML / ML->refine) | ML ms p50/p95 |",
             "|---|---:|---:|---:|---:|---:|---:|---|---|"]
    for name, s in report["evaluations"].items():
        lines.append(f"| {name} | {s['cases']:,} | {s['ok_classical']:,} | {s['ok_ml']:,} | {s['ok_ml_refine']:,} | {s['ok_hybrid']:,} | {s['rescued_by_ml']:,} | "
                     f"{s['wrong_classical']} / {s['wrong_ml']} / {s['wrong_ml_refine']} | {s['ml_inference_ms_p50']:.2f} / {s['ml_inference_ms_p95']:.2f} |")
    (data / "reports" / "ml.md").write_text("\n".join(lines) + "\n")
    try:
        from storage.r2 import R2
        r2 = R2(data)
        if r2.enabled:
            for f in files + [model, ROOT / "ml" / "models" / f"{model.stem}.card.json"]:
                r2.put_cas(f, "ml")
            c.log("ML: dataset and model uploaded to R2")
    except Exception as e:
        c.log(f"ML: R2 upload deferred ({e}); run ./ckt sync")
    c.log(f"ML report: {data / 'reports' / 'ml.md'}")
    return report
