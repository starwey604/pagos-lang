"""C4e3c: explicit generator selection, real cross-process reads and limits."""
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

BINARY, FIXTURE = sys.argv[1:3]
PROGRAM = (
    "fn table(x: u32) -> [u32; 4] { [for i in 2..6 { i * x + 1 }] } "
    "fn flag(x: bool) -> [bool; 4] { [for i in 2..6 { !x }] } "
    "fn signed(x: i8) -> [i8; 4] { [for i in 2..6 { -x }] } "
    "fn width(x: usize) -> [usize; 4] { [for i in 2..6 { x + 1usize }] } "
    "fn wide(x: u64) -> [u64; 4] { [for i in 2..6 { x }] } "
    "fn shadow(i: u32) -> [u32; 4] { [for i in i..i + 4 { i }] } "
    "fn pure() -> [u32; 4] { [for i in 2..6 { 42 }] } "
    "fn pair(x: u32, y: u32) -> [u32; 4] { [for i in 2..6 { i + x + y }] } "
    "fn scalar(x: u32) -> u32 { x * x } "
    "fn literal(x: u32) -> [u32; 4] { [x, x, x, x] } "
    "fn cast(x: u32) -> [u32; 4] { [for i in 2..6 { i as u32 }] } "
    "fn effect(x: u32) -> [u32; 4] { [for i in 2..6 { external_input() + i + x }] } "
    "fn memory(x: u32) -> [u32; 4] { [for i in 2..6 { volatile *(4096usize as *const u32) + i + x }] } "
    "let a = table(7); let b = table(8); let c = flag(true); let d = signed(7i8); "
    "let e = width(7usize); let f = wide(18446744073709551615u64); "
    "let g = shadow(7); let h = pure(); let j = pair(2, 5); "
    "let k = scalar(7); let l = literal(7); let m = cast(7);\n"
)


def files(root):
    return {str(p.relative_to(root)): (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
            for p in root.rglob("*") if p.is_file()}


def execute(args, codes=(0,)):
    p = subprocess.run([BINARY, *map(str, args)], text=True, capture_output=True, timeout=60)
    assert p.returncode in codes, (args, p.returncode, p.stdout, p.stderr)
    if p.returncode in (1, 2):
        assert not p.stdout
    return p


def decoded(p, status="verified"):
    result = json.loads(p.stdout)
    assert result["schema"] == 1 and result["type"] == "generator-replay"
    assert result["status"] == status and result["stage_runs"] == 0
    assert not result["continuation_available"]
    if status == "verified":
        assert result["verification_scope"] == "selected-element"
    else:
        assert result["restart_required"] and "result" not in result and "details" not in result
        assert "bounds" not in result and "iteration_value" not in result
    return result


with tempfile.TemporaryDirectory(prefix="pagos-generator-cli-") as tmp:
    root = Path(tmp)
    source, store = root / 'quoted "生成.pgs', root / "cache"
    source.write_text(PROGRAM)
    target = "riscv32-unknown-elf"

    def publish():
        return execute([f"--target={target}", "--stage-cache=read-write", f"--cache-dir={store}",
                        "--cache-evidence=generator-replay", "emit-hir", source])

    def arguments(name="table", values=("7",), element="1", extra=()):
        return [f"--target={target}", f"--replay-read={store}", f"--replay-function={name}",
                *[f"--replay-arg={v}" for v in values],
                *([] if element is None else [f"--replay-element={element}"]),
                "--max-fuel=0", *extra, "explain-replay", source]

    def replay(name="table", values=("7",), element="1", extra=(), codes=(0,)):
        return execute(arguments(name, values, element, extra), codes)

    baseline_hir = publish().stdout
    before = files(store)
    first_process = replay()
    first = decoded(first_process)
    assert first["result"] == {"type": "u32", "value": "22"}
    assert first["bounds"] == {"first": 2, "last": 6, "length": 4}
    assert first["element_index"] == 1 and first["iteration_value"] == 3
    assert first["iterator_slot"] == 1
    assert first["work"]["bound_nodes"] == 2 and first["work"]["element_nodes"] == 5
    assert first["work"]["fuel"] == 10
    assert [s["value"]["value"] for s in first["details"]] == ["3", "7", "21", "1", "22"]
    assert [s["operation"] for s in first["details"]] == ["input", "input", "multiply", "constant", "add"]
    assert [s["input_kind"] for s in first["details"]] == ["iterator", "capture", None, None, None]
    assert first["details"][0]["input_slot"] == first["iterator_slot"]
    assert first["details"][1]["input_slot"] == 0
    assert first["selection"] == {"first": 0, "count": 5, "total": 5, "complete": True}
    assert len(first["recorded_result"]) == 64
    assert first_process.stdout == replay().stdout
    last = decoded(replay(element="3"))
    assert last["result"]["value"] == "36" and last["iteration_value"] == 5
    assert last["work"] == first["work"] and last["descriptor"] == first["descriptor"]
    other = decoded(replay(values=("8",)))
    assert other["result"]["value"] == "25" and other["specialization"] != first["specialization"]
    assert decoded(replay("flag", ("true",)))["result"] == {"type": "bool", "value": "false"}
    assert decoded(replay("signed"))["result"] == {"type": "i8", "value": "-7"}
    assert decoded(replay("width"))["result"] == {"type": "usize", "value": "8"}
    assert decoded(replay("wide", ("18446744073709551615",)))["result"]["value"] == "18446744073709551615"
    shadow = decoded(replay("shadow"))
    assert shadow["bounds"] == {"first": 7, "last": 11, "length": 4} and shadow["result"]["value"] == "8"
    assert decoded(replay("pure", ()))["result"]["value"] == "42"
    assert decoded(replay("pair", ("2", "5")))["result"]["value"] == "10"
    scalar = json.loads(replay("scalar", element=None).stdout)
    assert scalar["type"] == "scalar-replay" and scalar["result"]["value"] == "49"

    selected = decoded(replay(extra=["--replay-first=4", "--replay-count=1", "--replay-output-records=1"]))
    assert selected["details"] == first["details"][-1:] and not selected["selection"]["complete"]
    assert selected["work"]["fuel"] == first["work"]["fuel"]
    empty = decoded(replay(extra=["--replay-count=0", "--replay-output-records=0"]))
    assert empty["details"] == [] and empty["result"] == first["result"]
    for fuel in range(10):
        fail = decoded(replay(extra=[f"--replay-fuel={fuel}"], codes=(3,)), "unavailable")
        assert fail["reason"] == "fuel-exhausted" and fail["work"]["fuel"] == fuel
        assert fail["phase"] in ("admission", "begin", "end", "element")
    assert decoded(replay(extra=["--replay-fuel=10"]))["result"] == first["result"]
    for extra, reason in [(["--replay-value-bytes=0"], "value-memory-limit"),
                          (["--replay-output-records=4"], "output-limit"),
                          (["--replay-detail-bytes=0"], "output-limit"),
                          (["--replay-first=6"], "invalid-selection"),
                          (["--replay-first=4", "--replay-count=2"], "invalid-selection")]:
        assert decoded(replay(extra=extra, codes=(3,)), "unavailable")["reason"] == reason
    for element in ("4", "4294967295"):
        assert decoded(replay(element=element, codes=(3,)), "unavailable")["reason"] == "invalid-selection"
    rendered_bytes = len(first_process.stdout.encode())
    assert replay(extra=[f"--replay-render-bytes={rendered_bytes}"]).stdout == first_process.stdout
    for amount in (0, 1, rendered_bytes - 1):
        fail = replay(extra=[f"--replay-render-bytes={amount}"], codes=(3,))
        assert not fail.stdout and "render byte limit" in fail.stderr
    for name in ("literal", "cast", "effect", "memory"):
        assert decoded(replay(name, codes=(3,)), "unavailable")["reason"] == "unsupported-source"
    assert decoded(replay("absent", codes=(3,)), "unavailable")["reason"] == "function-not-found"
    assert decoded(replay(values=("9",), codes=(3,)), "unavailable")["reason"] == "missing-descriptor"
    replay(element=None, codes=(2,))
    replay("scalar", codes=(2,))
    for element in ("-1", "4294967296", "1+1", "all", "1.5"):
        replay(element=element, codes=(2,))
    for name, values in [("table", ()), ("table", ("7", "8")), ("table", ("1+1",)),
                         ("signed", ("128",)), ("flag", ("1",))]:
        replay(name, values, codes=(2,))
    for extra in (["--stage-cache=read-only", f"--cache-dir={root / 'forbidden'}"],
                  ["--cache-evidence=generator-replay"], [f"--evidence-write={root / 'forbidden'}"],
                  [f"--query-dir={root / 'forbidden'}"], [f"--evidence-spill={root / 'forbidden'}"]):
        replay(extra=extra, codes=(2,))
    execute(["--replay-element=0", "check", source], (2,))
    execute(["--cache-evidence=generator-replay", "check", source], (2,))
    assert not (root / "forbidden").exists()
    assert files(store) == before, "successful or unavailable read wrote cache data"

    # All locations resolve against the current byte-based source map.
    shift = "// 移動\n\n"
    source.write_text(shift + PROGRAM)
    moved = decoded(replay())
    assert moved["descriptor"] == first["descriptor"] and moved["recorded_result"] == first["recorded_result"]
    assert moved["source"]["revision"] != first["source"]["revision"]
    for a, b in zip(first["details"], moved["details"]):
        assert b["site"]["begin"] == a["site"]["begin"] + len(shift.encode())
        assert b["site"]["line"] == a["site"]["line"] + 2
    assert (shift + PROGRAM).encode()[moved["iterator_site"]["begin"]:moved["iterator_site"]["end"]] == b"i"
    source.write_text(PROGRAM.replace("i * x + 1", "i * x - 1"))
    assert decoded(replay(codes=(3,)), "unavailable")["reason"] == "missing-descriptor"
    source.write_text(PROGRAM + "comptime let bad = external_input();")
    fail = replay(codes=(3,))
    decoded(fail, "unavailable")
    assert "E2001" not in fail.stderr and "E4004" not in fail.stderr
    source.write_text(PROGRAM)
    target = "riscv64-unknown-elf"
    assert decoded(replay(codes=(3,)), "unavailable")["reason"] == "missing-descriptor"
    target = "riscv32-unknown-elf"

    # Resealed artifacts retain valid CAS envelopes and joint evidence digests.
    descriptor = next(p for p in (store / "objects").rglob("*")
                      if p.is_file() and p.read_bytes()[8] == 16 and b"table" in p.read_bytes())
    manifests = {p: p.read_bytes() for p in (store / "queries").rglob("*.manifest")}
    for mode, reason, phase in [("value", "result-mismatch", "element"),
                                ("bounds", "bounds-mismatch", "begin"),
                                ("source", "invalid-evidence", "admission"),
                                ("version", "incompatible-descriptor", "admission")]:
        changed = subprocess.run([FIXTURE, str(store), str(descriptor), mode], text=True, capture_output=True, timeout=30)
        assert changed.returncode == 0, changed.stderr
        snapshot = files(store)
        fail = decoded(replay(values=(changed.stdout.strip(),), codes=(3,)), "unavailable")
        assert fail["reason"] == reason and fail["phase"] == phase, fail
        assert files(store) == snapshot, "failure repaired or rewrote cache"
        for path, raw in manifests.items():
            path.write_bytes(raw)
    descriptor_payload = descriptor.read_bytes()
    descriptor.write_bytes(descriptor_payload[:-1])
    snapshot = files(store)
    assert decoded(replay(values=(changed.stdout.strip(),), codes=(3,)), "unavailable")["reason"] == "corrupt-object"
    assert files(store) == snapshot
    descriptor.unlink()
    snapshot = files(store)
    assert decoded(replay(values=(changed.stdout.strip(),), codes=(3,)), "unavailable")["reason"] == "missing-object"
    assert files(store) == snapshot
    descriptor.write_bytes(descriptor_payload)

    for path in (store / "queries").rglob("*.manifest"):
        if b"generator-replay-evidence.v1" in path.read_bytes() and b"table" in path.read_bytes():
            path.unlink()
    snapshot = files(store)
    assert decoded(replay(codes=(3,)), "unavailable")["reason"] == "missing-descriptor"
    assert files(store) == snapshot
    hot = execute([f"--target={target}", "--stage-cache=read-only", f"--cache-dir={store}", "--cache-stats", "emit-hir", source])
    assert hot.stdout == baseline_hir and "replay-hits=0" in hot.stderr
    assert files(store) == snapshot
    assert publish().stdout == baseline_hir
    assert decoded(replay())["result"] == first["result"]
    snapshot = files(store)
    with open("/dev/full", "wb") as full:
        fail = subprocess.run([BINARY, *map(str, arguments())], stdout=full, stderr=subprocess.PIPE, text=True, timeout=30)
    assert fail.returncode == 1 and "output failed" in fail.stderr
    assert files(store) == snapshot

    target, store = "riscv64-unknown-elf", root / "cache64"
    publish()
    assert decoded(replay("width"))["result"] == {"type": "usize", "value": "8"}
    assert decoded(replay("width"))["specialization"]["key"] != first["specialization"]["key"]

    source.write_text("fn table(x: u32) -> [u32; 4096] { [for i in 100..4196 { i + x }] } let a = table(7);")
    target, store = "riscv32-unknown-elf", root / "large"
    publish()
    lo, hi = decoded(replay(element="0")), decoded(replay(element="4095"))
    assert lo["work"] == hi["work"] and hi["work"]["element_nodes"] == 3
    assert hi["result"]["value"] == "4202" and hi["iteration_value"] == 4195

    # SIGINT after ready; balanced source avoids deep ASTs during admission.
    def expression(depth):
        return "x" if not depth else f"({expression(depth - 1)} + {expression(depth - 1)})"

    source.write_text(f"fn table(x: u32) -> [u32; 2] {{ [for i in 2..4 {{ {expression(11)} }}] }} let a = table(7);")
    store = root / "cancel"
    publish()
    snapshot = files(store)
    process = subprocess.Popen([BINARY, *map(str, arguments(extra=["--replay-stats"]))],
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready = process.stderr.readline()
    assert "generator-replay: stage-runs=0 ready" in ready, ready
    process.send_signal(signal.SIGINT)
    stdout, stderr = process.communicate(timeout=30)
    assert process.returncode == 130, (process.returncode, stdout, ready + stderr)
    if stdout:
        result = decoded(subprocess.CompletedProcess([], 130, stdout, stderr), "unavailable")
        assert result["reason"] == "cancelled"
    assert files(store) == snapshot
print("generator CLI: cross-process verified selection, quotas, source binding, corruption, cancellation and read-only storage passed")
