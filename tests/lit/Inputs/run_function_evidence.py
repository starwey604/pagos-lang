"""C4a3 independent-process and lazy IO contract checks."""
import hashlib
import pathlib
import re
import shutil
import subprocess
import sys

binary = str(pathlib.Path(sys.argv[1]).resolve())
root = pathlib.Path(sys.argv[2]).resolve()
shutil.rmtree(root, ignore_errors=True)
root.mkdir(parents=True)
source, store = root / "module.pgs", root / "evidence"
body = "let n0 = x + 1; " + " ".join(f"let n{i} = n{i-1} + 1;" for i in range(1, 256)) + " n255"
source.write_text("fn a(x: u32) -> u32 { if x == 0 { 1 } else { x + 2 } } "
                  + "fn b(x: u32) -> u32 { " + body + " } "
                  + "let x = external_input(); let y = external_input(); let aa = a(x); let bb = b(y);")

def run(*args, ok=True):
    result = subprocess.run([binary, *args, "explain-provenance", str(source)], cwd=root, text=True, capture_output=True)
    assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
    return result

def read(mode="sources", selected="aa", ok=True):
    return run(f"--evidence-read={store}", f"--evidence-root={selected}",
               f"--evidence-mode={mode}", "--provenance-slice=1", "--provenance-query-stats", "--max-fuel=0", ok=ok)

def stats(result):
    return {key: int(value) for key, value in re.findall(r"([a-z-]+)=(\d+)", result.stderr)}

def files():
    return {str(path.relative_to(store)): (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns)
            for path in store.rglob("*") if path.is_file()}

exported = run(f"--evidence-write={store}", "--evidence-root=aa", "--provenance-query-stats")
assert stats(exported)["cold-objects"] == 2, exported.stderr
before = files()
for mode in ("sources", "witness", "topology"):
    actual = read(mode)
    again = read(mode)
    assert actual.stdout == again.stdout
    assert stats(actual)["cold-objects"] == 2, actual.stderr
    if mode == "sources":
        assert exported.stdout == actual.stdout
assert files() == before, "read changed storage"
assert "decision rule=" in read("decisions", "a").stdout
assert stats(read("decisions", "a"))["cold-objects"] == 0
source.write_text("// revised positions\n\n" + source.read_text())
shifted = read()
assert shifted.stdout != exported.stdout
assert int(re.search(r"external_input @(\d+)", shifted.stdout)[1]) == int(re.search(r"external_input @(\d+)", exported.stdout)[1]) + len("// revised positions\n\n")
assert files() == before, "source revision read wrote storage"
read(selected="missing-root", ok=False)
# CAS kind lives after magic/version. The largest detail is b's 256-step body.
details = [path for path in (store / "objects").rglob("*") if path.is_file() and path.read_bytes()[8] == 11]
large = max(details, key=lambda path: path.stat().st_size)
# Corrupt reachable cold objects without changing the validated hot closure.
saved = {path: path.read_bytes() for path in details if path != large}
for path, data in saved.items():
    path.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
assert "unavailable" in read(ok=False).stdout
assert stats(read("decisions", "a"))["cold-objects"] == 0
for path, data in saved.items():
    path.write_bytes(data)
large.unlink()
read()
assert "unavailable" in read(selected="bb", ok=False).stdout
for path in details:
    path.unlink(missing_ok=True)
assert stats(read("decisions", "a"))["cold-objects"] == 0
assert "unavailable" in read(ok=False).stdout
# Required summary loss is a load failure, independently of cold availability.
summary = next(path for path in (store / "objects").rglob("*") if path.is_file() and path.read_bytes()[8] == 10)
summary.unlink()
assert "load failed" in read(ok=False).stderr
missing = root / "absent"
run(f"--evidence-read={missing}", ok=False)
assert not missing.exists()
run(f"--evidence-read={store}", "--evidence-write=other", ok=False)
run(f"--evidence-read={store}", "--provenance-query=compressed", ok=False)
print("function evidence: independent reads, lazy siblings, scopes, revisions and failures verified")
