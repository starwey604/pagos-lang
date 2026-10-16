"""Real process boundaries for opt-in provenance query checkpoints."""

import collections
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*args, ok=True):
    process = subprocess.run([sys.argv[1], *map(str, args)], capture_output=True, text=True, timeout=20)
    if not ok:
        assert process.returncode != 0, process.stdout
        return process
    assert process.returncode == 0, process.stderr
    return json.loads(process.stdout)


def records(pages):
    return collections.Counter(record for page in pages for record in page["records"])


with tempfile.TemporaryDirectory(prefix="pagos-checkpoint-process-") as temporary:
    directory = Path(temporary)
    source = directory / "input.pgs"
    calls = ", ".join("outer(a)" if i % 2 else "outer(b)" for i in range(128))
    source.write_text(
        "fn inner(x: u32) -> u32 { x + external_input() } "
        "fn outer(x: u32) -> u32 { inner(x) } "
        "let a = external_input(); let b = external_input(); let aa = [" + calls + "];"
    )
    assert run("prepare", directory, source)["stage_runs"] == 1
    for mode in ("sources", "witness"):
        oracle = run("oracle", directory, source, mode)
        assert oracle["stage_runs"] == 0
        for steps in (0, 1, 2, 7, 30, 200, 10000):
            for acknowledgement in ("ack", "unacked"):
                saved = run("save", directory, source, mode, steps, acknowledgement)
                resumed = run("resume", directory, source, mode, saved["digest"], saved["size"])
                assert saved["stage_runs"] == resumed["stage_runs"] == 0
                assert resumed["result"] == oracle["result"], (mode, steps)
                assert resumed["work"] == oracle["work"], (mode, steps)
                assert resumed["issues"] == oracle["issues"]
                # Persisted consumer receipt table: redelivery uses the same
                # sequence and byte-for-byte records, not a new logical page.
                received = {}
                repeated = 0
                for page in saved["pages"] + resumed["pages"]:
                    sequence = page["sequence"]
                    if sequence in received:
                        assert received[sequence] == page
                        repeated += 1
                    else:
                        received[sequence] = page
                assert sorted(received) == list(range(1, len(received) + 1))
                assert records(received.values()) == records(oracle["pages"])
                if acknowledgement == "unacked" and saved["pages"]:
                    assert repeated == 1
                else:
                    assert repeated == 0
        print(f"{mode}: 14 cross-process checkpoint cases passed", flush=True)
    saved = run("save", directory, source, "sources", 7, "ack")
    original = source.read_text()
    source.write_text(original + " ")
    incompatible = run("resume", directory, source, "sources", saved["digest"], saved["size"], ok=False)
    assert "cannot resume" in incompatible.stderr
    source.write_text(original)
    object_file = directory / "tasks" / "objects" / saved["digest"][:2] / saved["digest"]
    content = object_file.read_bytes()
    object_file.write_bytes(content[:-1])
    assert "object unavailable" in run("resume", directory, source, "sources", saved["digest"], saved["size"], ok=False).stderr
    object_file.unlink()
    assert "object unavailable" in run("resume", directory, source, "sources", saved["digest"], saved["size"], ok=False).stderr
    assert not (directory / "tasks" / "queries").exists()
print("cross-process checkpoints, acknowledged delivery and rejection checks passed")
