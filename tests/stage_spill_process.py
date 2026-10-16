#!/usr/bin/env python3
"""Kill real Stage before/after sealing; reclaim only dead task namespaces."""
import json
import pathlib
import subprocess
import sys
import tempfile
import time

probe, compiler = map(lambda value: str(pathlib.Path(value).resolve()), sys.argv[1:3])


def run(action, root):
    result = subprocess.run([probe, action, str(root)], capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, (result.args, result.stdout, result.stderr)
    return json.loads(result.stdout)


def reclaim(root):
    result = subprocess.run([compiler, "reclaim-evidence", str(root)], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, (result.stdout, result.stderr)
    return dict((key, int(value)) for key, value in (item.split("=") for item in result.stdout.strip().split(": ", 1)[1].split(", ")))


with tempfile.TemporaryDirectory(prefix="pagos-spill-process-") as temporary:
    base = pathlib.Path(temporary)
    normal = run("normal", base / "normal")
    memory = run("memory", base / "memory")
    assert normal["available"] and normal["writes"] > 0
    assert (normal["signature"], normal["fuel"]) == (memory["signature"], memory["fuel"])
    assert not any((base / "normal" / "spill").iterdir())
    quota = run("quota", base / "quota")
    assert not quota["available"] and quota["written"] > 0 and quota["blocked"] > 0
    assert quota["writes"] == 0 and not (base / "quota" / "semantic").exists()
    assert (quota["signature"], quota["fuel"]) == (memory["signature"], memory["fuel"])
    assert not any((base / "quota" / "spill").iterdir())
    for boundary in ("before", "after"):
        root = base / boundary
        root.mkdir()
        child = subprocess.Popen([probe, boundary, str(root)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            ready = root / "ready"
            deadline = time.monotonic() + 5
            while not ready.exists() or not ready.read_text():
                assert child.poll() is None, child.communicate()
                assert time.monotonic() < deadline, "Stage readiness timeout"
                time.sleep(0.005)
            session = pathlib.Path(ready.read_text())
            assert session.parent == root / "spill"
            assert not (root / "semantic").exists(), "pending semantic cache escaped before proof"
            counts = reclaim(root / "spill")
            assert counts["active"] == 1 and counts["reclaimed"] == counts["errors"] == 0, counts
            objects = list(session.glob("objects/*/*"))
            if boundary == "before":
                assert not objects and any((session / "tmp").iterdir()), "expected half-written temporary"
            else:
                assert objects, "expected atomically sealed leaf"
                # CAS publication uses a hard link. The completed temporary
                # may still exist until the interrupted write call unwinds.
                assert all(path.read_bytes() == objects[0].read_bytes() for path in (session / "tmp").iterdir())
            child.kill()
            child.communicate(timeout=5)
            assert child.returncode < 0
            assert session.is_dir(), "SIGKILL unexpectedly ran RAII cleanup"
            # A new successful task must not adopt or delete the dead session.
            orphan = {str(path.relative_to(session)): path.read_bytes() for path in session.rglob("*") if path.is_file()}
            recovered = run("normal", root)
            assert recovered["available"] and recovered["writes"] > 0
            assert {str(path.relative_to(session)): path.read_bytes() for path in session.rglob("*") if path.is_file()} == orphan
            assert not list(session.glob("queries/**/*.manifest"))
            unknown = root / "spill" / "pagos-evidence-unknown"
            unknown.mkdir()
            (unknown / "session.lock").write_text("unknown protocol")
            counts = reclaim(root / "spill")
            assert counts["reclaimed"] == 1 and counts["skipped"] == 1 and counts["errors"] == 0, counts
            assert not session.exists() and unknown.exists()
            assert reclaim(root / "spill")["reclaimed"] == 0
        finally:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=5)
    print("Stage SIGKILL before/after seal, live-lock protection, quota and explicit reclamation passed")
