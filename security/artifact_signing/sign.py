"""Artifact signing with OpenSSH signatures (ssh-keygen -Y, namespace 'ckey-test').

A campaign key (Ed25519) is created at BASELINE_LOCKED under
campaign/keys/; its public half and an allowed_signers file are recorded in
the lock. Every freeze record, manifest roll-up and certification decision is
signed. If ssh-keygen is unavailable the artifact is recorded as UNSIGNED;
nothing ever claims a signature that does not exist.
"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

NAMESPACE = "ckey-test"
IDENTITY = "ckey-test-campaign"


def available() -> bool:
    return shutil.which("ssh-keygen") is not None


def ensure_key(key_dir: Path) -> Path | None:
    if not available():
        return None
    key_dir.mkdir(parents=True, exist_ok=True)
    key = key_dir / "campaign_ed25519"
    if not key.exists():
        subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-C", IDENTITY, "-f", str(key)], check=True)
    pub = key.with_suffix(".pub").read_text().strip()
    (key_dir / "allowed_signers").write_text(f"{IDENTITY} {pub}\n")
    return key


def sign(path: Path, key_dir: Path) -> str:
    key = key_dir / "campaign_ed25519"
    if not available() or not key.exists():
        return "UNSIGNED"
    sig = path.with_name(path.name + ".sig")
    sig.unlink(missing_ok=True)
    subprocess.run(["ssh-keygen", "-q", "-Y", "sign", "-f", str(key), "-n", NAMESPACE, str(path)], check=True)
    return str(sig)


def verify(path: Path, key_dir: Path) -> bool:
    sig = path.with_name(path.name + ".sig")
    signers = key_dir / "allowed_signers"
    if not available() or not sig.exists() or not signers.exists():
        return False
    with open(path, "rb") as fh:
        p = subprocess.run(["ssh-keygen", "-Y", "verify", "-f", str(signers), "-I", IDENTITY, "-n", NAMESPACE, "-s", str(sig)],
                           stdin=fh, capture_output=True)
    return p.returncode == 0
