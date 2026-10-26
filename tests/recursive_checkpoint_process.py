"""C4d3a resident recursive execution state crosses actual process boundaries."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*args, ok=True):
    result = subprocess.run([sys.argv[1], *map(str, args)], capture_output=True,
                            text=True, timeout=30)
    if not ok:
        assert result.returncode != 0 and not result.stdout, result
        return result
    assert result.returncode == 0, (args, result.stderr)
    return json.loads(result.stdout)


def files(root):
    return {str(path.relative_to(root)): (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns)
            for path in root.rglob("*") if path.is_file()}


with tempfile.TemporaryDirectory(prefix="pagos-recursive-process-") as temporary:
    directory = Path(temporary)
    source = directory / "input.pgs"
    text = (
        "fn rotate(x: u32, y: u32) -> u32 { if x == 0 { y } else { rotate(y, x - 1) } } "
        "fn read(x: u32) -> u32 { if x == 0 { external_input() } else { read(x - 1) } } "
        "let a = external_input(); let b = external_input(); let aa = [rotate(a, b), read(a)];"
    )
    source.write_text(text)
    assert run("prepare", directory, source)["stage_runs"] == 1
    evidence = files(directory / "evidence")
    for name in ("rotate", "read"):
        for channel in ("value", "effects"):
            oracle = run("rec-oracle", directory, source, name, channel)
            assert oracle["stage_runs"] == 0
            for phase in range(7):
                saved = run("rec-save-phase", directory, source, name, channel,
                            phase, "cancel" if phase % 2 else "keep")
                assert saved["phase"] == phase and saved["stage_runs"] == 0
                before = files(directory / "recursive-state")
                resumed = run("rec-resume", directory, source, name, channel,
                              saved["digest"], saved["size"])
                assert resumed == oracle, (name, channel, phase)
                assert files(directory / "recursive-state") == before, "restore wrote state"
            print(f"{name}/{channel}: seven saved phases resumed with identical work", flush=True)
    assert files(directory / "evidence") == evidence, "queries changed semantic evidence"
    saved = run("rec-save", directory, source, "rotate", "value", 7, "keep")
    assert "cannot resume" in run("rec-resume", directory, source, "rotate", "effects",
        saved["digest"], saved["size"], ok=False).stderr
    assert "cannot resume" in run("rec-resume", directory, source, "read", "value",
        saved["digest"], saved["size"], ok=False).stderr
    source.write_text(text + " ")
    assert "cannot resume" in run("rec-resume", directory, source, "rotate", "value",
        saved["digest"], saved["size"], ok=False).stderr
    source.write_text(text)
    obj = directory / "recursive-state" / "objects" / saved["digest"][:2] / saved["digest"]
    data = obj.read_bytes()
    obj.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
    assert "object unavailable" in run("rec-resume", directory, source, "rotate", "value",
        saved["digest"], saved["size"], ok=False).stderr
    obj.unlink()
    assert "object unavailable" in run("rec-resume", directory, source, "rotate", "value",
        saved["digest"], saved["size"], ok=False).stderr
    assert files(directory / "evidence") == evidence
    assert not (directory / "recursive-state" / "queries").exists()
    assert not (directory / "sessions").exists(), "resident checkpoint is not a TaskStore task"
print("recursive checkpoint phases, bindings, read-only CAS restore and failures verified")
