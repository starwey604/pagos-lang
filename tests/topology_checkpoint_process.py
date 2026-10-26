"""Symbolic topology/formula tasks resume across real process boundaries."""

import collections
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*args, ok=True):
    result = subprocess.run([sys.argv[1], *map(str, args)], capture_output=True,
                            text=True, timeout=25)
    if not ok:
        assert result.returncode != 0, result.stdout
        return result
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def files(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


def records(pages):
    return collections.Counter(record for page in pages for record in page["records"])


with tempfile.TemporaryDirectory(prefix="pagos-topology-checkpoint-") as temporary:
    directory = Path(temporary)
    source = directory / "input.pgs"
    source.write_text(
        "fn inner(x: u32) -> u32 { x + external_input() } "
        "fn outer(x: u32) -> u32 { inner(x) } "
        "let a = external_input(); let b = external_input(); let aa = [" +
        ", ".join("outer(a)" if i % 2 else "outer(b)" for i in range(32)) + "];"
    )
    assert run("prepare", directory, source)["stage_runs"] == 1
    evidence = files(directory / "evidence")
    oracle = run("topo-oracle", directory, source, "topology")
    assert oracle["stage_runs"] == 0 and oracle["work"][3] > 0
    for prefix in ("topo-", "topo-seg-"):
        for action, steps in (("topo-save", 0), ("topo-save", 7),
                              ("topo-save-formula", 7), ("topo-save", 10000)):
            for ack in ("ack", "unacked"):
                saved = run(action.replace("topo-", prefix, 1), directory, source, steps, ack)
                if action == "topo-save-formula":
                    assert 0 < saved["work"][5] < oracle["work"][5], "must cut during preparation"
                    assert saved["work"][3] == 0, "completed formula inventory must stay unpublished"
                before = files(directory / "sessions")
                restored = run(prefix + "resume", directory, source, "topology")
                assert restored["stage_runs"] == 0
                assert restored["result"] == oracle["result"], (action, steps, ack)
                assert restored["work"] == oracle["work"], (action, steps, ack)
                assert files(directory / "sessions") == before, "read-only restore wrote task state"
                received = {}
                repeated = 0
                for page in saved["pages"] + restored["pages"]:
                    sequence = page["sequence"]
                    if sequence in received:
                        assert received[sequence] == page
                        repeated += 1
                    else:
                        received[sequence] = page
                assert sorted(received) == list(range(1, len(received) + 1))
                assert records(received.values()) == records(oracle["pages"])
                assert repeated == (ack == "unacked" and bool(saved["pages"]) and action != "topo-save-formula")
                assert run("seg-cleanup-all", directory, source)["reclaimed"] == 1
    # Source-only edits invalidate the saved task even if semantic IDs still fit.
    run("topo-save-formula", directory, source, 3, "ack")
    original = source.read_text()
    source.write_text(original + " ")
    assert "cannot resume" in run("topo-resume", directory, source, "topology", ok=False).stderr
    source.write_text(original)
    assert run("seg-cleanup-all", directory, source)["reclaimed"] == 1
    run("topo-seg-save-formula", directory, source, 7, "ack")
    leaves = [path for path in (directory / "sessions").rglob("objects/*/*")
              if path.is_file() and path.read_bytes()[8] == 13]
    assert leaves
    leaves[0].unlink()
    assert "cannot resume" in run("topo-seg-resume", directory, source, "topology", ok=False).stderr
    assert files(directory / "evidence") == evidence
    assert run("seg-cleanup-all", directory, source)["reclaimed"] == 1
print("symbolic topology, partial formula preparation and acknowledged pages resume without Stage")
