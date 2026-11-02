"""C4e3c: native effect trace and residual output remain unchanged by replay."""
import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import subprocess
from string import Template
import tempfile


@dataclass(frozen=True)
class EffectFixture:
    program: str
    entry: str
    callback: str
    expected_trace: str


TABLE = """
fn table(x: u32) -> [u32; 4] { [for i in 2..6 { i * x + 1 }] }
"""
FIXTURES = {
    "minimal-c": EffectFixture(
        program="extern fn observe(x: u32) -> u32;\n" + TABLE + """
fn effects(x: u32) -> [u32; 4] { [for i in 2..6 { observe(i + x) }] }
export fn entry() -> u32 {
    let data = table(7);
    let work = effects(7);
    observe(data[2] + work[0])
}
""",
        entry="entry",
        callback="uint32_t observe(uint32_t x) { record_effect(x); return x; }",
        expected_trace="9\n10\n11\n12\n38\n",
    ),
    # The synthetic entry/builtin input boundary works without admitting the
    # explicit extern/export C ABI, which still requires target validation.
    "builtin-entry": EffectFixture(
        program=TABLE + """
fn effects(x: u32) -> [u32; 4] { [for i in 2..6 { external_input() + i + x }] }
let data = table(7); let work = effects(7); data[2] + work[0];
""",
        entry="pagos_main",
        callback="""uint32_t pagos_external_input(void) {
    static unsigned calls;
    record_effect(++calls);
    return 0;
}""",
        expected_trace="1\n2\n3\n4\n",
    ),
}

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary")
parser.add_argument("clang")
parser.add_argument("--abi", choices=FIXTURES, required=True)
parser.add_argument("--executable-suffix", default="")
args_config = parser.parse_args()
fixture = FIXTURES[args_config.abi]

RUNTIME = Template(r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern uint32_t ${entry}(void);
static void record_effect(uint32_t x) {
    FILE *input = fopen(getenv("PAGOS_EFFECT_INPUT"), "r");
    unsigned marker = 0;
    if (!input || fscanf(input, "%u", &marker) != 1 || marker != 17) abort();
    if (fclose(input)) abort();
    FILE *stream = fopen(getenv("PAGOS_EFFECT_LOG"), "a");
    if (!stream) abort();
    fprintf(stream, "%u\n", x);
    if (fclose(stream)) abort();
}
${callback}
int main(void) {
    uint32_t result = ${entry}();
    printf("%u\n", result);
    return 0;
}
""").substitute(entry=fixture.entry, callback=fixture.callback)


def files(root):
    return {str(p.relative_to(root)): (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
            for p in root.rglob("*") if p.is_file()}


with tempfile.TemporaryDirectory(prefix="pagos-generator-effects-") as tmp:
    root = Path(tmp)
    source, store, log, runtime = root / "effects.pgs", root / "cache", root / "effects.log", root / "runtime.c"
    input_file = root / "input.txt"
    source.write_text(fixture.program, encoding="utf-8", newline="\n")
    runtime.write_text(RUNTIME, encoding="utf-8", newline="\n")
    log.write_text("", encoding="utf-8", newline="\n")
    input_file.write_text("17\n", encoding="utf-8", newline="\n")
    input_snapshot = (input_file.read_bytes(), input_file.stat().st_mtime_ns)
    environment = dict(os.environ, PAGOS_EFFECT_LOG=str(log), PAGOS_EFFECT_INPUT=str(input_file))
    # Both the compiler and linker use their native host target.

    def execute(args, codes=(0,)):
        result = subprocess.run([args_config.binary, *map(str, args)], text=True, capture_output=True, env=environment, timeout=60)
        assert result.returncode in codes, (args, result.returncode, result.stdout, result.stderr)
        return result

    def replay(name="table", extra=(), codes=(0,)):
        return execute([f"--replay-read={store}", f"--replay-function={name}",
                        "--replay-arg=7", "--replay-element=2", "--max-fuel=0", *extra,
                        "explain-replay", source], codes)

    def generated(mode, name, evidence="value"):
        options = [f"--stage-cache={mode}"]
        if mode != "off":
            options += [f"--cache-dir={store}", f"--cache-evidence={evidence}"]
        llvm = execute([*options, "emit-llvm", source]).stdout
        obj, exe = root / f"{name}.o", root / (name + args_config.executable_suffix)
        execute([*options, "emit-obj", source, "-o", obj])
        linked = subprocess.run([args_config.clang, str(obj), str(runtime), "-o", str(exe)], capture_output=True, text=True, timeout=60)
        assert linked.returncode == 0, linked.stderr
        return llvm, exe

    off_ir, off = generated("off", "off")
    cold_ir, cold = generated("read-write", "cold", "generator-replay")
    assert off_ir == cold_ir and not log.read_text(), "compilation executed external effects"

    # A real host executable demonstrates the expected dynamic call sequence.
    def runtime_check(exe):
        log.write_text("", encoding="utf-8", newline="\n")
        result = subprocess.run([str(exe)], capture_output=True, text=True, env=environment, timeout=30)
        assert result.returncode == 0 and result.stdout == "38\n", (result.returncode, result.stdout, result.stderr)
        assert log.read_text() == fixture.expected_trace, log.read_text()

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
print("generator effects: native external-call/builtin-input trace and residual LLVM unchanged; CLI success/failure/unsupported paths have no effects or cache writes")
