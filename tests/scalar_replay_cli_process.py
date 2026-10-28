"""C4e2b: real CLI publication, explicit read/replay, bounded verified JSON."""
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile

BINARY, FIXTURE = sys.argv[1:3]
PROGRAM = (
    "fn square(x: u32) -> u32 { x * x } "
    "fn signed(x: i8) -> i8 { -x } "
    "fn flag(x: bool) -> bool { !x } "
    "fn width(x: usize) -> usize { x + 1usize } "
    "fn wide(x: u64) -> u64 { x } "
    "fn pair(x: u32, y: u32) -> u32 { x + y } "
    "fn pure() -> u32 { 9 } "
    "fn cast(x: u32) -> u32 { x as u32 } "
    "fn branch(x: u32) -> u32 { if x == 0 { 1 } else { x } } "
    "fn short(x: bool) -> bool { x && true } "
    "fn side(x: u32) -> u32 { external_input() + x } "
    "let a = square(7); let b = square(8); let c = signed(7i8); "
    "let d = flag(true); let e = width(7usize); "
    "let f = wide(18446744073709551615u64); let g = pair(2, 5); "
    "let h = pure(); let i = cast(7); let j = branch(7); let k = short(true); "
    "let runtime_value = side(7);\n"
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
    value = json.loads(p.stdout)
    assert value["schema"] == 1 and value["type"] == "scalar-replay"
    assert value["status"] == status and value["stage_runs"] == 0
    assert not value["continuation_available"]
    if status != "verified":
        assert value["restart_required"] and "details" not in value and "result" not in value
    return value


with tempfile.TemporaryDirectory(prefix="pagos-replay-cli-") as tmp:
    root = Path(tmp)
    source, store = root / 'quoted "输入.pgs', root / "cache"
    source.write_text(PROGRAM)
    target = "riscv32-unknown-elf"

    def publish():
        return execute([f"--target={target}", "--stage-cache=read-write", f"--cache-dir={store}",
                        "--cache-evidence=scalar-replay", "emit-hir", source])

    def arguments(name="square", values=("7",), extra=()):
        return [f"--target={target}", f"--replay-read={store}", f"--replay-function={name}",
                *[f"--replay-arg={value}" for value in values], "--max-fuel=0", *extra,
                "explain-replay", source]

    def replay(name="square", values=("7",), extra=(), codes=(0,)):
        return execute(arguments(name, values, extra), codes)

    baseline_hir = publish().stdout
    before = files(store)
    first_process = replay()
    first = decoded(first_process)
    assert first["result"] == {"type": "u32", "value": "49"}
    assert first["work"]["nodes"] == 3 and first["work"]["fuel"] == 4
    assert [step["value"]["value"] for step in first["details"]] == ["7", "7", "49"]
    assert [step["operation"] for step in first["details"]] == ["input", "input", "multiply"]
    assert first["details"][-1]["operands"] == [0, 1]
    assert first["details"][0]["input_slot"] == first["details"][1]["input_slot"] == 0
    assert first["selection"] == {"first": 0, "count": 3, "total": 3, "complete": True}
    assert first["source"]["path"] == str(source)
    for step in first["details"]:
        span = step["site"]
        assert 0 <= span["begin"] <= span["end"] <= len(PROGRAM)
        assert span["line"] == 1 and span["column"] == span["begin"] + 1
    assert first_process.stdout == replay().stdout
    other = decoded(replay(values=("8",)))
    assert other["result"]["value"] == "64"
    assert other["specialization"]["key"] != first["specialization"]["key"]
    assert decoded(replay("signed"))["result"] == {"type": "i8", "value": "-7"}
    assert decoded(replay("flag", ("true",)))["result"] == {"type": "bool", "value": "false"}
    assert decoded(replay("wide", ("18446744073709551615",)))["result"]["value"] == "18446744073709551615"
    assert decoded(replay("pair", ("2", "5")))["result"]["value"] == "7"
    assert decoded(replay("pure", ()))["result"]["value"] == "9"
    assert decoded(replay("width"))["result"] == {"type": "usize", "value": "8"}

    root_only = decoded(replay(extra=["--replay-first=2", "--replay-count=1", "--replay-output-records=1"]))
    assert root_only["details"] == first["details"][-1:]
    assert root_only["work"]["nodes"] == first["work"]["nodes"]
    assert root_only["work"]["fuel"] == first["work"]["fuel"]
    assert not root_only["selection"]["complete"]
    empty = decoded(replay(extra=["--replay-count=0", "--replay-output-records=0"]))
    assert not empty["details"] and empty["result"] == first["result"]
    for fuel in range(4):
        fail = decoded(replay(extra=[f"--replay-fuel={fuel}"], codes=(3,)), "unavailable")
        assert fail["reason"] == "fuel-exhausted" and fail["work"]["fuel"] == fuel
    assert decoded(replay(extra=["--replay-fuel=4"]))["result"] == first["result"]
    for extra, reason in [(["--replay-value-bytes=0"], "value-memory-limit"),
                          (["--replay-output-records=2"], "output-limit"),
                          (["--replay-detail-bytes=0"], "output-limit"),
                          (["--replay-first=4"], "invalid-selection"),
                          (["--replay-first=2", "--replay-count=2"], "invalid-selection")]:
        fail = decoded(replay(extra=extra, codes=(3,)), "unavailable")
        assert fail["reason"] == reason
    count = len(first_process.stdout.encode())
    assert replay(extra=[f"--replay-render-bytes={count}"]).stdout == first_process.stdout
    for amount in (0, 1, count - 1):
        fail = replay(extra=[f"--replay-render-bytes={amount}"], codes=(3,))
        assert not fail.stdout and "render byte limit" in fail.stderr
    for name, values in [("cast", ("7",)), ("branch", ("7",)), ("short", ("true",)), ("side", ("7",))]:
        assert decoded(replay(name, values, codes=(3,)), "unavailable")["reason"] == "unsupported-source"
    assert decoded(replay("absent", codes=(3,)), "unavailable")["reason"] == "function-not-found"
    assert decoded(replay(values=("9",), codes=(3,)), "unavailable")["reason"] == "missing-descriptor"
    for name, values in [("square", ()), ("square", ("7", "8")), ("square", ("-1",)),
                         ("square", ("1+2",)), ("signed", ("128",)), ("flag", ("1",))]:
        replay(name, values, codes=(2,))
    assert files(store) == before, "read/replay or failure wrote cache data"

    # Position rebinding preserves the mathematical record and specialization.
    shift = "// moved\n\n"
    source.write_text(shift + PROGRAM)
    moved = decoded(replay())
    assert moved["descriptor"] == first["descriptor"]
    assert moved["specialization"] == first["specialization"]
    assert moved["source"]["revision"] != first["source"]["revision"]
    for a, b in zip(first["details"], moved["details"]):
        assert b["site"]["begin"] == a["site"]["begin"] + len(shift)
        assert b["site"]["line"] == a["site"]["line"] + 2
    source.write_text(PROGRAM.replace("x * x", "x + x"))
    assert decoded(replay(codes=(3,)), "unavailable")["reason"] == "missing-descriptor"
    source.write_text(PROGRAM + "comptime let bad = external_input();")
    fail = replay(codes=(3,))
    decoded(fail, "unavailable")
    assert "E2001" not in fail.stderr and "E4004" not in fail.stderr
    source.write_text(PROGRAM)
    target = "riscv64-unknown-elf"
    assert decoded(replay(codes=(3,)), "unavailable")["reason"] == "missing-descriptor"
    target = "riscv32-unknown-elf"

    # Genuine CLI cache corruption, including an internally resealed false value.
    descriptors = [p for p in (store / "objects").rglob("*")
                   if p.is_file() and p.read_bytes()[8] == 15 and b"square" in p.read_bytes()]
    assert descriptors
    manifests = {p: p.read_bytes() for p in (store / "queries").rglob("*.manifest")}
    forged = subprocess.run([FIXTURE, str(store), str(descriptors[0])], text=True, capture_output=True, timeout=30)
    assert forged.returncode == 0, forged.stderr
    forged_snapshot = files(store)
    fail = decoded(replay(values=(forged.stdout.strip(),), codes=(3,)), "unavailable")
    assert fail["reason"] == "result-mismatch"
    assert files(store) == forged_snapshot, "mismatch repaired/overwrote the cache"
    for path, payload in manifests.items():
        path.write_bytes(payload)
    payloads = {p: p.read_bytes() for p in descriptors}
    for path in descriptors:
        path.write_bytes(payloads[path][:-1])
    corrupted = files(store)
    assert decoded(replay(codes=(3,)), "unavailable")["reason"] == "corrupt-object"
    assert files(store) == corrupted
    for path, payload in payloads.items():
        path.write_bytes(payload)
    for path in descriptors:
        path.unlink()
    missing_objects = files(store)
    assert decoded(replay(codes=(3,)), "unavailable")["reason"] == "missing-object"
    assert files(store) == missing_objects
    for path, payload in payloads.items():
        path.write_bytes(payload)
    for path in (store / "queries").rglob("*.manifest"):
        raw = path.read_bytes()
        if b"scalar-replay-evidence.v1" in raw and b"square" in raw:
            path.unlink()
    old = files(store)
    assert decoded(replay(codes=(3,)), "unavailable")["reason"] == "missing-descriptor"
    assert files(store) == old

    # Publication is still explicit, with source-body execution disabled for reads.
    for extra in (["--stage-cache=read-write", f"--cache-dir={root / 'forbidden'}"],
                  [f"--snapshot-write={root / 'forbidden'}"], [f"--evidence-write={root / 'forbidden'}"],
                  [f"--query-dir={root / 'forbidden'}"], ["--replay-fuel=-1"], ["--replay-count=oops"],
                  ["--replay-unknown=1"]):
        replay(extra=extra, codes=(2,))
    assert not (root / "forbidden").exists()
    execute([f"--replay-read={store}", "check", source], (2,))
    execute(["explain-replay", source], (2,))

    output_snapshot = files(store)
    with open("/dev/full", "wb") as full:
        failed_output = subprocess.run([BINARY, *map(str, arguments("pure", ()))],
                                       stdout=full, stderr=subprocess.PIPE, text=True, timeout=30)
    assert failed_output.returncode == 1 and "output failed" in failed_output.stderr
    assert files(store) == output_snapshot

    # A separate target generation demonstrates actual usize width selection.
    store = root / "cache64"
    target = "riscv64-unknown-elf"
    publish()
    positive64 = decoded(replay("width"))
    assert positive64["result"] == {"type": "usize", "value": "8"}
    assert positive64["specialization"]["key"] != decoded(replay("square"))["specialization"]["key"]

    # SIGINT after the explicit ready event must not publish verified facts.
    # A balanced expression makes admission long enough without deep ASTs.
    def expression(depth):
        return "x" if not depth else f"({expression(depth - 1)} + {expression(depth - 1)})"
    source.write_text(f"fn square(x: u32) -> u32 {{ {expression(11)} }} let a = square(7);")
    target = "riscv32-unknown-elf"
    store = root / "cancel"
    publish()
    cancel_snapshot = files(store)
    process = subprocess.Popen([BINARY, *map(str, arguments(extra=["--replay-stats"]))],
                               text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready = process.stderr.readline()
    assert "stage-runs=0 ready" in ready, ready
    process.send_signal(signal.SIGINT)
    stdout, stderr = process.communicate(timeout=30)
    assert process.returncode == 130, (process.returncode, stdout, ready + stderr)
    if stdout:
        result = json.loads(stdout)
        assert result["status"] == "unavailable" and result["reason"] == "cancelled"
        assert "details" not in result and "result" not in result
    assert files(store) == cancel_snapshot
print("scalar replay CLI: typed selection, verified source details, quotas, corruption, cancellation and no Stage fallback verified")
