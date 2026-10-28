"""C4e3c: native effect trace and residual output remain unchanged by replay."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

BINARY, CLANG = sys.argv[1:3]
PROGRAM = """
extern fn observe(x: u32) -> u32;
fn table(x: u32) -> [u32; 4] { [for i in 2..6 { i * x + 1 }] }
fn effects(x: u32) -> [u32; 4] { [for i in 2..6 { observe(i + x) }] }
export fn entry() -> u32 {
    let data = table(7);
    let work = effects(7);
    observe(data[2] + work[0])
}
"""
RUNTIME = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern uint32_t entry(void);
uint32_t observe(uint32_t x) {
    FILE *input = fopen(getenv("PAGOS_EFFECT_INPUT"), "r");
    unsigned marker = 0;
    if (!input || fscanf(input, "%u", &marker) != 1 || marker != 17) abort();
    if (fclose(input)) abort();
    FILE *stream = fopen(getenv("PAGOS_EFFECT_LOG"), "a");
    if (!stream) abort();
    fprintf(stream, "%u\n", x);
    if (fclose(stream)) abort();
    return x;
}
int main(void) {
    uint32_t result = entry();
    printf("%u\n", result);
    return 0;
}
"""


def files(root):
    return {str(p.relative_to(root)): (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
            for p in root.rglob("*") if p.is_file()}


with tempfile.TemporaryDirectory(prefix="pagos-generator-effects-") as tmp:
    root = Path(tmp)
    source, store, log, runtime = root / "effects.pgs", root / "cache", root / "effects.log", root / "runtime.c"
    input_file = root / "input.txt"
    source.write_text(PROGRAM)
    runtime.write_text(RUNTIME)
    log.write_text("")
    input_file.write_text("17\n")
    input_snapshot = (input_file.read_bytes(), input_file.stat().st_mtime_ns)
    environment = dict(os.environ, PAGOS_EFFECT_LOG=str(log), PAGOS_EFFECT_INPUT=str(input_file))
    target = "--target=x86_64-unknown-linux-gnu"

    def execute(args, codes=(0,)):
        result = subprocess.run([BINARY, *map(str, args)], text=True, capture_output=True, env=environment, timeout=60)
        assert result.returncode in codes, (args, result.returncode, result.stdout, result.stderr)
        return result

    def replay(name="table", extra=(), codes=(0,)):
        return execute([target, f"--replay-read={store}", f"--replay-function={name}",
                        "--replay-arg=7", "--replay-element=2", "--max-fuel=0", *extra,
                        "explain-replay", source], codes)

    def generated(mode, name, evidence="value"):
        options = [target, f"--stage-cache={mode}"]
        if mode != "off":
            options += [f"--cache-dir={store}", f"--cache-evidence={evidence}"]
        llvm = execute([*options, "emit-llvm", source]).stdout
        obj, exe = root / f"{name}.o", root / name
        execute([*options, "emit-obj", source, "-o", obj])
        linked = subprocess.run([CLANG, str(obj), str(runtime), "-o", str(exe)], capture_output=True, text=True, timeout=60)
        assert linked.returncode == 0, linked.stderr
        return llvm, exe

    off_ir, off = generated("off", "off")
    cold_ir, cold = generated("read-write", "cold", "generator-replay")
    assert off_ir == cold_ir and not log.read_text(), "compilation executed external effects"

    # A real host executable demonstrates the expected dynamic call sequence.
    def runtime_check(exe):
        log.write_text("")
        result = subprocess.run([str(exe)], capture_output=True, text=True, env=environment, timeout=30)
        assert result.returncode == 0 and result.stdout == "38\n", (result.returncode, result.stdout, result.stderr)
        assert log.read_text() == "9\n10\n11\n12\n38\n", log.read_text()

    runtime_check(off)
    runtime_check(cold)
    log_snapshot = (log.read_bytes(), log.stat().st_mtime_ns)
    snapshot = files(store)
    for extra, code in [((), 0), (["--replay-count=0"], 0), (["--replay-fuel=0"], 3),
                        (["--replay-output-records=0"], 3), (["--replay-render-bytes=0"], 3)]:
        result = replay(extra=extra, codes=(code,))
        if code == 0:
            value = json.loads(result.stdout)
            assert value["stage_runs"] == 0 and value["result"]["value"] == "29"
        assert (log.read_bytes(), log.stat().st_mtime_ns) == log_snapshot
        assert (input_file.read_bytes(), input_file.stat().st_mtime_ns) == input_snapshot
        assert files(store) == snapshot
    unsupported = json.loads(replay("effects", codes=(3,)).stdout)
    assert unsupported["reason"] == "unsupported-source" and "result" not in unsupported
    assert (log.read_bytes(), log.stat().st_mtime_ns) == log_snapshot and files(store) == snapshot
    assert (input_file.read_bytes(), input_file.stat().st_mtime_ns) == input_snapshot
    hot_ir, hot = generated("read-only", "hot", "generator-replay")
    assert hot_ir == off_ir and files(store) == snapshot
    assert (log.read_bytes(), log.stat().st_mtime_ns) == log_snapshot
    runtime_check(hot)
print("generator effects: native trace 9,10,11,12,38 and residual LLVM unchanged; CLI success/failure/unsupported paths have no effects or cache writes")
