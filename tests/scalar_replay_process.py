"""C4e1b real Stage publication and independent, non-executing source-bound reads."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

PROBE, COMPILER = sys.argv[1:3]
PROGRAM = "fn square(x: usize) -> usize { x * x } let a = square(7usize); let b = square(7usize);\n"


def files(root):
    return {str(path.relative_to(root)): (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns)
            for path in root.rglob("*") if path.is_file()}


with tempfile.TemporaryDirectory(prefix="pagos-scalar-replay-process-") as temporary:
    root = Path(temporary)
    source, store = root / "input.pgs", root / "store"
    source.write_text(PROGRAM)

    def probe(action, bits="32", build="probe-build-v1", codes=(0,), fuel=None):
        args = [PROBE, action, str(store), str(source), bits, build]
        if fuel is not None:
            args.append(str(fuel))
        process = subprocess.run(args,
                                 text=True, capture_output=True, timeout=30)
        assert process.returncode in codes, (action, process.returncode, process.stdout, process.stderr)
        if process.returncode:
            assert not process.stdout
            return process
        return json.loads(process.stdout)

    cold = probe("publish")
    assert cold["replay_writes"] == 1 and cold["evaluations"] == 1
    snapshot = files(store)
    first = probe("read")
    assert first["stage_runs"] == 0 and first["value"] == 49 and first["nodes"] == 3
    for _ in range(3):
        assert probe("read") == first
        executed = probe("execute")
        assert executed["stage_runs"] == 0 and executed["nodes"] == first["nodes"]
        assert executed["fuel"] == first["nodes"] + 1
        assert executed["descriptor"] == first["descriptor"] and executed["value"] == first["value"]
        assert executed["details"] == [{"node": 0, "value": 7}, {"node": 1, "value": 7}, {"node": 2, "value": 49}]
    for fuel in range(first["nodes"] + 1):
        failed = probe("execute", codes=(3,), fuel=fuel)
        assert "execution unavailable" in failed.stderr
    assert probe("execute", fuel=first["nodes"] + 1) == executed

    warm = probe("require")
    assert warm["replay_hits"] == warm["hits"] == 1 and warm["evaluations"] == 0
    assert warm["fuel"] == cold["fuel"]
    assert files(store) == snapshot

    shift = "// moved source\n\n"
    source.write_text(shift + PROGRAM)
    moved = probe("read")
    assert moved["object"] == first["object"] and moved["descriptor"] == first["descriptor"]
    assert moved["begin"] == first["begin"] + len(shift)
    assert moved["end"] == first["end"] + len(shift)
    assert files(store) == snapshot
    probe("read", bits="64", codes=(3,))
    probe("read", build="different-compiler", codes=(3,))
    source.write_text(PROGRAM.replace("x * x", "x + x"))
    probe("read", codes=(3,))
    source.write_text(PROGRAM.replace("7usize", "8usize"))
    probe("read", codes=(3,))
    source.write_text(PROGRAM + "fn unrelated() -> u32 { 9 }\n")
    probe("read", codes=(3,))
    source.write_text(PROGRAM)
    assert files(store) == snapshot

    # A corrupt/missing required replay object cannot become a P1 hit, while
    # the separate C2 value index remains usable. Read failures create no files.
    hex_digest = first["object"]
    object_path = store / "objects" / hex_digest[:2] / hex_digest
    original = object_path.read_bytes()
    object_path.write_bytes(original[:-1])
    corrupt_snapshot = files(store)
    probe("read", codes=(3,))
    fallback = probe("require")
    assert fallback["hits"] == 0 and fallback["evaluations"] == 1
    assert fallback["fuel"] == cold["fuel"]
    assert probe("value")["hits"] == 1
    assert files(store) == corrupt_snapshot
    object_path.unlink()
    missing_snapshot = files(store)
    probe("read", codes=(3,))
    assert probe("require")["hits"] == 0 and files(store) == missing_snapshot
    object_path.write_bytes(original)
    assert probe("read") == first

    # Old value-only generations do not silently acquire replay capability.
    manifests = [path for path in (store / "queries").rglob("*.manifest")
                 if b"scalar-replay-evidence.v1" in path.read_bytes()]
    assert len(manifests) == 1
    manifests[0].unlink()
    old_snapshot = files(store)
    probe("read", codes=(3,))
    assert probe("value")["hits"] == 1
    assert probe("require")["hits"] == 0
    assert files(store) == old_snapshot
    upgrade = probe("publish")  # Default value hits do not fabricate evidence.
    assert upgrade["hits"] == 1 and upgrade["replay_writes"] == 0
    assert files(store) == old_snapshot
    # The compiler's explicit requirement recomputes a real body and upgrades.
    # Probe action publish uses the default; use a fresh source generation for
    # the independent reader's positive post-loss record.
    source.write_text(PROGRAM.replace("x * x", "x * (x + 0usize)"))
    assert probe("publish")["replay_writes"] == 1
    assert probe("read")["stage_runs"] == 0

    # Real CLI capability parsing, budget preservation and default text remain.
    cli_store = root / "cli"
    base = ["--target=riscv32-unknown-elf", "--cache-stats", f"--cache-dir={cli_store}"]

    def compiler(mode, *extra, codes=(0,)):
        process = subprocess.run([COMPILER, *base, f"--stage-cache={mode}", *extra,
                                  "emit-hir", str(source)], text=True, capture_output=True, timeout=30)
        assert process.returncode in codes, (process.returncode, process.stdout, process.stderr)
        return process

    off = compiler("off")
    cold_cli = compiler("read-write", "--cache-evidence=scalar-replay")
    hot_cli = compiler("read-only", "--cache-evidence=scalar-replay")
    assert off.stdout == cold_cli.stdout == hot_cli.stdout
    assert "replay-writes=1" in cold_cli.stderr and "replay-hits=1" in hot_cli.stderr
    cli_before = files(cli_store)
    failed = compiler("read-only", "--cache-evidence=scalar-replay", "--max-fuel=1", codes=(1,))
    assert "E4004" in failed.stderr and files(cli_store) == cli_before
    # A fresh 64-bit production/consumption pair also executes without Stage.
    store = root / "store64"
    cold64 = probe("publish", bits="64")
    assert cold64["replay_writes"] == 1
    snapshot64 = files(store)
    replay64 = probe("execute", bits="64")
    assert replay64["stage_runs"] == 0 and replay64["value"] == 49
    assert replay64["fuel"] == replay64["nodes"] + 1
    assert files(store) == snapshot64

    compiler("off", "--cache-evidence=scalar-replay", codes=(2,))
    compiler("read-only", "--cache-evidence=other", codes=(2,))
print("scalar replay: real Stage, independent reads, source/target bindings, missing/corrupt evidence and CLI capability verified")
