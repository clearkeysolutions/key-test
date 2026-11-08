"""Cloudflare R2 storage for campaign artifacts (S3-compatible API via boto3).

Cost design (R2: ~$0.015/GB-month, no egress, ~$4.50 per million writes):
* NO per-case objects. A sealed shard (1,000 cases) is uploaded as its gzip
  plan/results/traces files: about 3 objects per 1,000 cases, ~5,000 objects
  and ~3-6 GB for the whole 1.6M campaign (cents per month).
* Content-addressed: objects live at cas/sha256/<aa>/<sha256><ext>, so nothing is
  ever uploaded twice and every object is verifiable from the hashes in git.
* Per-campaign indexes (campaigns/<campaign_id>/...) map plan sets and shards to hashes.
* Images are bundled (tar.gz per plan set) and only failures/samples are kept.

Credentials come from the environment (never from git):
  R2_ACCOUNT_ID, R2_ACCESS_KEY_ID, R2_SECRET_ACCESS_KEY  (optionally R2_BUCKET)
scripts/server/r2.env.example shows the file the server scripts source.
"""
from __future__ import annotations

import hashlib
import json
import os
import tarfile
import tempfile
import time
from pathlib import Path

from generator.common import file_sha256, load_yaml


class R2:
    def __init__(self, data_dir: Path):
        cfg = load_yaml("campaign.yaml").get("storage", {}).get("r2", {})
        self.bucket = os.environ.get("R2_BUCKET", cfg.get("bucket", "ckey-hardening-test"))
        self.account = os.environ.get("R2_ACCOUNT_ID", "")
        self.data = data_dir
        self.log_path = data_dir / "storage" / "uploaded.jsonl"
        self._client = None
        self._done: set[str] | None = None
        lock = data_dir / "state" / "lock.json"
        self.campaign_id = file_sha256(lock)[:16] if lock.exists() else "unlocked"

    @property
    def enabled(self) -> bool:
        return bool(self.account and os.environ.get("R2_ACCESS_KEY_ID") and os.environ.get("R2_SECRET_ACCESS_KEY"))

    def client(self):
        if self._client is None:
            import boto3  # optional dependency (requirements.txt)
            from botocore.config import Config
            self._client = boto3.client("s3", endpoint_url=f"https://{self.account}.r2.cloudflarestorage.com",
                                        aws_access_key_id=os.environ["R2_ACCESS_KEY_ID"], aws_secret_access_key=os.environ["R2_SECRET_ACCESS_KEY"],
                                        region_name="auto", config=Config(retries={"max_attempts": 8, "mode": "adaptive"}))
        return self._client

    def done(self) -> set[str]:
        if self._done is None:
            self._done = set()
            if self.log_path.exists():
                for line in self.log_path.read_text().splitlines():
                    try:
                        self._done.add(json.loads(line)["sha256"])
                    except Exception:
                        pass
        return self._done

    def put_cas(self, path: Path, kind: str) -> dict:
        """Upload a file content-addressed; idempotent (skips hashes already uploaded)."""
        h = file_sha256(path)
        ext = "".join(path.suffixes[-2:]) if path.name.endswith(".jsonl.gz") else path.suffix
        key = f"cas/sha256/{h[:2]}/{h}{ext}"
        rec = {"sha256": h, "key": key, "bytes": path.stat().st_size, "kind": kind, "name": path.name, "time": time.time()}
        if h in self.done():
            return rec
        if not self.enabled:
            raise RuntimeError("R2 not configured (R2_ACCOUNT_ID / R2_ACCESS_KEY_ID / R2_SECRET_ACCESS_KEY)")
        self.client().upload_file(str(path), self.bucket, key, ExtraArgs={"Metadata": {"sha256": h, "kind": kind}})
        head = self.client().head_object(Bucket=self.bucket, Key=key)
        if head["ContentLength"] != rec["bytes"]:
            raise RuntimeError(f"R2 size mismatch for {key}")
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        with open(self.log_path, "a") as fh:
            fh.write(json.dumps(rec) + "\n")
        self.done().add(h)
        return rec

    def put_bytes(self, key: str, data: bytes) -> None:
        if self.enabled:
            self.client().put_object(Bucket=self.bucket, Key=f"campaigns/{self.campaign_id}/{key}", Body=data)

    # ----------------------------------------------------------------------------------
    def upload_shard(self, plan_set: str, stem: str) -> list[dict]:
        out = []
        for kind, d in (("plan", "plans"), ("results", "results"), ("traces", "traces")):
            f = self.data / d / plan_set / f"{stem}.jsonl.gz"
            if f.exists():
                out.append(self.put_cas(f, f"{kind}:{plan_set}"))
        return out

    def sync(self, log=print) -> dict:
        """Upload every sealed shard, manifests, reports and image bundles not yet stored."""
        from runners.sharding.supervisor import read_jsonl
        stats = {"uploaded": 0, "bytes": 0, "skipped": 0, "failed": 0}
        index = []
        for mf in sorted((self.data / "manifests").glob("*.jsonl")):
            ps = mf.stem
            for rec in read_jsonl(mf):
                try:
                    before = len(self.done())
                    recs = self.upload_shard(ps, rec["stem"])
                    new = len(self.done()) - before
                    stats["uploaded"] += new
                    stats["skipped"] += len(recs) - new
                    index.append({"plan_set": ps, "stem": rec["stem"], "objects": recs})
                except Exception as e:
                    stats["failed"] += 1
                    log(f"  R2 upload failed for {ps}/{rec['stem']}: {e}")
            self.put_bytes(f"manifests/{ps}.jsonl", mf.read_bytes())
        # Image bundles: one tar.gz per plan set (failures + samples only, capped by the worker).
        for d in sorted((self.data / "images").glob("*")) if (self.data / "images").exists() else []:
            if not d.is_dir():
                continue
            with tempfile.TemporaryDirectory() as tmp:
                tarp = Path(tmp) / f"images-{d.name}.tar.gz"
                with tarfile.open(tarp, "w:gz") as tf:
                    for f in sorted(d.rglob("*.png")):
                        info = tf.gettarinfo(str(f), arcname=str(f.relative_to(d)))
                        info.mtime = 0
                        with open(f, "rb") as fh:
                            tf.addfile(info, fh)
                try:
                    self.put_cas(tarp, f"images:{d.name}")
                except Exception as e:
                    stats["failed"] += 1
                    log(f"  R2 image bundle failed for {d.name}: {e}")
        for f in sorted((self.data / "reports").glob("*")) if (self.data / "reports").exists() else []:
            if f.is_file():
                try:
                    self.put_cas(f, "report")
                except Exception as e:
                    stats["failed"] += 1
                    log(f"  R2 report upload failed {f.name}: {e}")
        for f in [self.data / "state" / "lock.json", self.data / "state" / "transitions.jsonl"]:
            if f.exists():
                self.put_bytes(f"state/{f.name}", f.read_bytes())
        self.put_bytes("index.json", json.dumps(index).encode())
        stats["bytes"] = sum(json.loads(l)["bytes"] for l in self.log_path.read_text().splitlines()) if self.log_path.exists() else 0
        return stats
