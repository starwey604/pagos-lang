"""Segmented task heads, process death, read-only leases and explicit cleanup."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

probe = sys.argv[1]


def run(*args, ok=True):
    result = subprocess.run([probe, *map(str, args)], capture_output=True, text=True, timeout=20)
    if not ok:
        assert result.returncode != 0, result.stdout
        return result
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def wait_ready(process, ready):
    deadline = time.monotonic() + 20
    while not ready.exists():
        assert process.poll() is None, process.communicate()
        assert time.monotonic() < deadline, "probe did not reach the fault boundary"
        time.sleep(0.01)


def files(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


with tempfile.TemporaryDirectory(prefix="pagos-query-task-process-") as temporary:
    directory = Path(temporary)
    source = directory / "input.pgs"
    source.write_text(
        "fn inner(x: u32) -> u32 { x + external_input() } "
        "fn outer(x: u32) -> u32 { inner(x) } "
        "let a = external_input(); let b = external_input(); let aa = [" +
        ", ".join("outer(a)" if i % 2 else "outer(b)" for i in range(32)) + "];"
    )
    run("prepare", directory, source)
    semantic_files = files(directory / "evidence")
    for mode in ("sources", "witness"):
        oracle = run("oracle", directory, source, mode)
        for ack in ("ack", "unacked"):
            saved = run("seg-save", directory, source, mode, 10000, ack)
            assert saved["released"]
            before = files(directory / "sessions")
            restored = run("seg-resume", directory, source, mode)
            assert restored["result"] == oracle["result"]
            assert restored["work"] == oracle["work"]
            assert restored["stage_runs"] == 0
            assert files(directory / "sessions") == before, "read-only restore wrote to its task"
            if ack == "unacked" and saved["pages"]:
                assert saved["pages"][-1] == restored["pages"][0]
            assert run("seg-cleanup-all", directory, source)["reclaimed"] == 1
    # Active paging resumes in another process with bounded append/read buffers.
    # Use both a mid-traversal cut and a completed or unconfirmed delivery cut.
    for mode in ("sources", "witness"):
        oracle = run("oracle", directory, source, mode)
        for steps in (7, 10000):
            for ack in ("ack", "unacked"):
                saved = run("page-save", directory, source, mode, steps, ack)
                assert saved["released"] and saved["paged"]
                restored = run("page-resume", directory, source, mode)
                assert restored["paged"] and restored["reads"] > 0
                assert restored["result"] == oracle["result"]
                assert restored["work"] == oracle["work"]
                assert restored["stage_runs"] == 0
                if ack == "unacked" and saved["pages"]:
                    assert saved["pages"][-1] == restored["pages"][0]
                assert run("seg-cleanup-all", directory, source)["reclaimed"] == 1
    oracle = run("oracle", directory, source, "sources")
    for boundary in ("before", "after"):
        initial = run("seg-save", directory, source, "sources", 1, "ack")
        ready = directory / "ready"
        ready.unlink(missing_ok=True)
        process = subprocess.Popen([probe, "seg-crash-" + boundary, str(directory), str(source), "sources", "7", "ack"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            wait_ready(process, ready)
            assert run("seg-cleanup-all", directory, source)["active"] == 1
            process.kill()
            process.communicate(timeout=10)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=10)
        current = run("seg-head", directory, source)
        assert current["generation"] == initial["generation"] + (boundary == "after")
        restored = run("seg-resume", directory, source, "sources")
        assert restored["result"] == oracle["result"]
        assert restored["work"] == oracle["work"]
        assert run("seg-cleanup", directory, source)["skipped"] == 1  # Resumable work is retained.
        assert run("seg-cleanup-all", directory, source)["reclaimed"] == 1
    # Cancel is saved as a terminal head, then explicitly resumed in a new process.
    run("seg-cancel", directory, source, "sources", 7, "ack")
    assert run("seg-resume", directory, source, "sources")["result"] == oracle["result"]
    assert run("seg-cleanup", directory, source)["reclaimed"] == 1
    run("seg-save", directory, source, "sources", 10000, "ack")
    ready = directory / "ready"
    ready.unlink(missing_ok=True)
    reader = subprocess.Popen([probe, "seg-hold-reader", str(directory), str(source)],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        wait_ready(reader, ready)
        # Reader opens/closes a second same-process handle before announcing readiness.
        assert run("seg-cleanup", directory, source)["active"] == 1
        reader.kill()
        reader.communicate(timeout=10)
    finally:
        if reader.poll() is None:
            reader.kill()
            reader.communicate(timeout=10)
    assert run("seg-cleanup", directory, source)["reclaimed"] == 1
    run("seg-save", directory, source, "sources", 7, "ack")
    parts = [path for path in (directory / "sessions").rglob("objects/*/*")
             if path.is_file() and path.read_bytes()[8] == 13]
    assert len(parts) > 1
    parts[-1].unlink()
    assert "cannot resume" in run("seg-resume", directory, source, "sources", ok=False).stderr
    assert run("seg-cleanup-all", directory, source)["reclaimed"] == 1
    assert files(directory / "evidence") == semantic_files
    assert not list((directory / "sessions").rglob("queries"))
print("segmented and paged tasks, atomic heads, process death and lifetime cleanup passed")
