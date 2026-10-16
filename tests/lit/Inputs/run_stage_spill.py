"""C4b2: real Stage, explicit temporary spill and residual execution."""
import pathlib
import re
import shutil
import subprocess
import sys

binary, clang = str(pathlib.Path(sys.argv[1]).resolve()), sys.argv[2]
root = pathlib.Path(sys.argv[3]).resolve()
shutil.rmtree(root, ignore_errors=True)
root.mkdir(parents=True)
source = root / "program.pgs"
body = "let n0 = x + 1; " + " ".join(f"let n{i} = n{i-1} + 1;" for i in range(1, 64)) + " n63"
source.write_text("fn square(x: u32) -> u32 { x * x } "
    "comptime let known = square(7); fn big(x: u32) -> u32 { " + body + " } "
    "let x = external_input(); let y = external_input(); let arr = [7, x]; "
    "comptime let fixed = arr[0]; let a = big(x); let b = big(y); a + b + fixed + known;")
spill = ["--evidence-spill=spill", "--spill-block-bytes=128", "--spill-resident-bytes=8192", "--spill-cache-bytes=512", "--spill-stats"]

def run(command, *flags, ok=True):
    result = subprocess.run([binary, *flags, command, str(source)], cwd=root, capture_output=True, text=True, timeout=30)
    assert (result.returncode == 0) == ok, (flags, result.stdout, result.stderr)
    if "--evidence-spill=spill" in flags:
        assert not any((root / "spill").iterdir()) if (root / "spill").exists() else True, "temporary namespace leaked"
    return result

def stats(result):
    return {key: int(value) for key, value in re.findall(r"([a-z-]+)=(\d+)", result.stderr)}

for command in ("check", "emit-hir", "emit-mir", "emit-llvm", "explain-stage", "explain-provenance"):
    memory = run(command)
    actual = run(command, *spill)
    assert memory.stdout == actual.stdout, command
    assert stats(actual)["materialized"] == 0
    assert stats(actual)["written"] > 1
    assert stats(actual)["fallback"] == 0
    assert stats(actual)["peak-owned"] <= 8192
    failed = run(command, *spill, "--spill-resident-bytes=0", ok=command != "explain-provenance")
    assert stats(failed)["available"] == 0
    if command != "explain-provenance":
        assert failed.stdout == memory.stdout
    else:
        assert "provenance: unavailable" in failed.stdout
for mode in ("resumable", "topology"):
    result = run("explain-provenance", *spill, f"--provenance-query={mode}", "--provenance-slice=1")
    assert stats(result)["materialized"] == 0
assert stats(run("explain-provenance", *spill, "--provenance-query=compressed"))["materialized"] == 1
# Execute ordinary and failed-evidence residual output with identical input reads.
runtime = pathlib.Path(__file__).with_name("runtime.c").resolve()
for index, flags in enumerate(([], spill, [*spill, "--spill-resident-bytes=0"])):
    emitted = run("emit-llvm", *flags)
    ir, exe = root / f"case{index}.ll", root / f"case{index}"
    ir.write_text(emitted.stdout)
    subprocess.run([clang, str(ir), str(runtime), "-o", str(exe)], check=True, capture_output=True)
    actual = subprocess.run([str(exe), "3", "5"], check=True, text=True, capture_output=True)
    assert actual.stdout == "input=3\ninput=5\nresult=192\n", actual.stdout
# An explicitly supplied relative path must spill, not silently fall back.
assert stats(run("check", *spill))["written"] > 0
blocker = root / "not-a-directory"
blocker.write_text("keep")
base = run("emit-hir")
fallback = run("emit-hir", f"--evidence-spill={blocker}", "--spill-block-bytes=128", "--spill-stats")
assert fallback.stdout == base.stdout
assert stats(fallback)["available"] == 1 and stats(fallback)["fallback"] > 0
strict = run("emit-hir", f"--evidence-spill={blocker}", "--spill-block-bytes=128", "--spill-on-error=unavailable", "--spill-stats")
assert strict.stdout == base.stdout and stats(strict)["available"] == 0
assert blocker.read_text() == "keep"
# Semantic-cache reads/off never acquire spill permission implicitly.
absent = root / "semantic-absent"
run("check", f"--cache-dir={absent}", "--stage-cache=off")
run("check", f"--cache-dir={absent}", "--stage-cache=read-only", *spill)
assert not absent.exists()
late = run("check", *spill, "--spill-disk-bytes=100", "--spill-on-error=unavailable",
    f"--cache-dir={absent}", "--stage-cache=read-write", "--cache-stats")
assert stats(late)["writes"] == 0 and stats(late)["blocked-writes"] > 0
assert not absent.exists()
run("check", "--spill-stats", ok=False)
run("check", "--spill-block-bytes=0", "--evidence-spill=spill", ok=False)
run("explain-provenance", "--evidence-spill=spill", "--evidence-read=other", ok=False)
assert not (root / "other").exists()
print("stage spill: native queries, budgets, diagnostics, residual execution and publication guards verified")
