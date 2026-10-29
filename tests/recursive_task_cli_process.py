"""C4d3c: real-process continuation, immutable final receipts and maintenance."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

BINARY = sys.argv[1]
PROGRAM = (
    "fn rotate(x: u32, y: u32) -> u32 { if x == 0 { y } else { rotate(y, x - 1) } } "
    "fn even(n: u32) -> u32 { if n == 0 { 1 } else { odd(n - 1) } } "
    "fn odd(n: u32) -> u32 { if n == 0 { 0 } else { even(n - 1) } } "
    "fn input(n: u32) -> u32 { if n == 0 { external_input() } else { input(n - 1) } } "
    "fn scale(k: u32, x: u32) -> u32 { k * x } "
    "let x = external_input(); let y = external_input(); "
    "let aa = [rotate(x, y), even(x), input(x), scale(4, x), scale(5, x)];"
)


def files(root):
    return {str(path.relative_to(root)): (hashlib.sha256(path.read_bytes()).hexdigest(),
                                         path.stat().st_mtime_ns)
            for path in root.rglob("*") if path.is_file()}


def active_segments(directory):
    # TaskHead v1: PGC1/kind 14, version, task ID, generation, status,
    # manifest kind, manifest digest, size and checksum. Select objects from
    # the head's manifest, never from arbitrary/orphaned CAS generations.
    head = (directory / "head").read_bytes()
    assert len(head) == 127 and head[:4] == b"PGC1" and head[8] == 14
    assert int.from_bytes(head[9:13], "little") == 1 and head[54] == 14
    digest = head[55:87].hex()
    envelope = (directory / "objects" / digest[:2] / digest).read_bytes()
    assert envelope[:4] == b"PGO1" and envelope[8] == 14
    manifest = envelope[49:]
    return [path for path in (directory / "objects").rglob("*")
            if path.is_file() and path.read_bytes()[8] == 13
            and bytes.fromhex(path.name) in manifest]


def decode(process):
    rows = [json.loads(line) for line in process.stdout.splitlines()]
    assert rows and rows[-1]["type"] == "status", process
    assert all(row["schema"] == 1 for row in rows)
    deliveries = rows[:-1]
    assert len(deliveries) <= 1
    if deliveries:
        assert deliveries[0]["type"] == "recursive-result"
        assert deliveries[0]["sequence"] == 1
    status = rows[-1]
    assert status["mode"] == "recursive-summary"
    assert status["continuation_available"] and not status["restart_required"]
    assert status["stage_runs"] == 0
    return deliveries, status


with tempfile.TemporaryDirectory(prefix="pagos-recursive-task-cli-") as temporary:
    root = Path(temporary)
    source, evidence, tasks = root / "input.pgs", root / "evidence", root / "tasks"
    source.write_text(PROGRAM)
    base = ["--target=riscv32-unknown-elf"]

    def run(args, codes=(0, 3)):
        process = subprocess.run([BINARY, *map(str, args)], capture_output=True,
                                 text=True, timeout=40)
        assert process.returncode in codes, (args, process.returncode, process.stdout, process.stderr)
        if process.returncode in (1, 2):
            assert not process.stdout, "failure leaked partial JSON"
        return process

    published = run([*base, f"--evidence-write={evidence}", "--evidence-mode=summaries",
                     "explain-provenance", source], (0,))
    catalog = json.loads(published.stdout)["summaries"]
    immutable_evidence = files(evidence)

    def arguments(name="rotate", channel="value", task=None, action="start", slices="all",
                  size=4096, extra=(), read_only=False, key=None, directory=tasks):
        args = [*base, "--max-fuel=0", f"--evidence-read={evidence}",
                "--evidence-mode=recursive-summary", f"--evidence-channel={channel}",
                f"--query-dir={directory}", f"--query-action={action}",
                f"--provenance-slice={size}", "--query-segment-bytes=128"]
        args.append(f"--evidence-summary-key={key}" if key else f"--evidence-summary={name}")
        if task:
            args.append(f"--query-id={task}")
        if read_only:
            args.append("--query-read-only")
        elif slices is not None:
            args.append(f"--query-max-slices={slices}")
        return [*args, *extra, "explain-provenance", source]

    def query(*args, codes=(0, 3), **kwargs):
        return run(arguments(*args, **kwargs), codes)

    def cleanup(discard=False):
        result = run(["reclaim-queries", tasks, *(["--discard-resumable"] if discard else [])], (0,))
        return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", result.stdout)}

    # Every command starts a new process; source analysis cannot run with zero fuel.
    for name in ("rotate", "even", "input"):
        for channel in ("value", "effects"):
            initial = query(name, channel, size=7, slices=1)
            deliveries, status = decode(initial)
            task = status["task"]
            assert not deliveries and status["state"] == "paused"
            assert status["work"]["steps"] == 7 and status["offloaded"]
            snapshot = files(tasks)
            _, ro = decode(query(name, channel, task, "resume", read_only=True))
            assert ro["work"] == status["work"] and ro["generation"] == status["generation"]
            assert files(tasks) == snapshot
            query(name, channel, task, "resume", extra=["--query-ack=1"], codes=(1,))
            assert files(tasks) == snapshot
            # Cancelled work is retained and can still be explicitly resumed.
            _, cancelled = decode(query(name, channel, task, "cancel"))
            assert cancelled["state"] == "cancelled" and cancelled["work"] == status["work"]
            snapshot = files(tasks)
            _, ro = decode(query(name, channel, task, "resume", read_only=True))
            assert ro["state"] == "cancelled" and files(tasks) == snapshot
            _, short = decode(query(name, channel, task, "resume", size=17, slices=2,
                                    extra=["--recursive-max-steps=19"]))
            assert short["work"]["steps"] == 26
            # Step budget is additional work for this invocation, not a total ceiling.
            _, short2 = decode(query(name, channel, task, "resume", size=17, slices=2,
                                     extra=["--recursive-max-steps=19"]))
            assert short2["work"]["steps"] == 45
            final = query(name, channel, task, "resume")
            deliveries, status = decode(final)
            assert len(deliveries) == 1 and status["state"] == "awaiting-delivery"
            assert status["acknowledge"] == 1 and status["acknowledged"] == 0
            delivered_bytes = final.stdout.splitlines()[0]
            result = deliveries[0]["result"]
            assert result["fixed_point"] and result["proof_available"]
            assert not result["concrete_sources_complete"]
            assert result["precision"] == ("conservative" if name == "input" else "exact-for-recorded-model")
            fresh = run([*base, "--max-fuel=0", f"--evidence-read={evidence}",
                         "--evidence-mode=recursive-summary", f"--evidence-summary={name}",
                         f"--evidence-channel={channel}", "explain-provenance", source], (0,))
            oracle = json.loads(fresh.stdout)["results"][0]
            actual = json.loads(json.dumps(result))
            for value in (actual, oracle):
                value["work"].pop("slices")
            assert actual == oracle
            assert cleanup()["skipped"] == 1
            snapshot = files(tasks)
            for read_only in (True, False, True):
                replay = query(name, channel, task, "resume", read_only=read_only)
                repeated, rs = decode(replay)
                assert repeated == deliveries and replay.stdout.splitlines()[0] == delivered_bytes
                assert rs["work"] == status["work"] and rs["generation"] == status["generation"]
                assert files(tasks) == snapshot, "replay wrote task files"
            query(name, channel, task, "cancel", codes=(1,))
            query(name, channel, task, "resume", extra=["--query-ack=2"], codes=(1,))
            query(name, channel, task, "resume", read_only=True, extra=["--query-ack=1"], codes=(2,))
            assert files(tasks) == snapshot
            # Failed confirmation never makes the saved pending receipt disappear.
            query(name, channel, task, "resume", extra=["--query-ack=1", "--query-disk-bytes=256"], codes=(1,))
            repeated, rs = decode(query(name, channel, task, "resume", read_only=True))
            assert repeated == deliveries and rs["state"] == "awaiting-delivery"
            _, acked = decode(query(name, channel, task, "resume", extra=["--query-ack=1"]))
            assert acked["state"] == "completed" and acked["acknowledge"] is None
            assert acked["acknowledged"] == 1 and acked["work"] == status["work"]
            snapshot = files(tasks)
            again, duplicate = decode(query(name, channel, task, "resume", extra=["--query-ack=1"]))
            assert not again and duplicate["generation"] == acked["generation"]
            assert files(tasks) == snapshot
            again, ro = decode(query(name, channel, task, "resume", read_only=True))
            assert not again and ro["acknowledged"] == 1 and files(tasks) == snapshot
            assert cleanup()["reclaimed"] == 1
            assert files(evidence) == immutable_evidence

    # A failed writable continuation retains the last committed logical work.
    _, status = decode(query(slices=1, size=7))
    task = status["task"]
    before = files(tasks)
    query(task=task, action="resume", extra=["--query-disk-bytes=256"], codes=(1,))
    assert files(tasks) == before
    _, restored = decode(query(task=task, action="resume", read_only=True))
    assert restored["work"] == status["work"]
    # Wrong source/positions, actual specialization, channel or target must not write.
    query(task=task, action="resume", channel="effects", codes=(1,))
    query(name="input", task=task, action="resume", codes=(1,))
    source.write_text("// changed revision\n" + PROGRAM)
    query(task=task, action="resume", codes=(1,))
    source.write_text(PROGRAM)
    query(task=task, action="resume", extra=["--target=x86_64-unknown-linux-gnu"], codes=(1,))
    assert files(tasks) == before
    assert cleanup()["skipped"] == 1
    assert cleanup(True)["reclaimed"] == 1

    # Ambiguous names are rejected before a task namespace is created.
    absent = root / "absent"
    query(name="scale", directory=absent, codes=(1,))
    assert not absent.exists()
    scales = [entry for entry in catalog if entry["name"] == "scale"]
    assert len(scales) == 2
    _, status = decode(query(key=scales[0]["key"], slices=0))
    task = status["task"]
    snapshot = files(tasks)
    query(key=scales[1]["key"], task=task, action="resume", codes=(1,))
    assert files(tasks) == snapshot
    assert cleanup(True)["reclaimed"] == 1

    # A write/start invocation has Stage work, but delivery bytes omit that
    # invocation-local field and are identical on a later independent read.
    write_id = "e" * 64
    write_args = [arg for arg in arguments(task=write_id)
                  if arg != "--max-fuel=0" and not str(arg).startswith("--evidence-read=")]
    write_args.insert(0, f"--evidence-write={evidence}")
    started = run(write_args, (3,))
    rows = [json.loads(line) for line in started.stdout.splitlines()]
    assert rows[-1]["stage_runs"] == 1 and rows[-1]["state"] == "awaiting-delivery"
    repeated = query(task=write_id, action="resume", read_only=True)
    _, read_status = decode(repeated)
    assert read_status["stage_runs"] == 0
    assert started.stdout.splitlines()[0] == repeated.stdout.splitlines()[0]
    assert cleanup(True)["reclaimed"] == 1
    assert files(evidence) == immutable_evidence

    # Output failure happens after publication, so a new process replays it.
    full_id = "f" * 64
    with open("/dev/full", "wb") as full:
        failed = subprocess.run([BINARY, *map(str, arguments(task=full_id))],
                                stdout=full, stderr=subprocess.PIPE, text=True, timeout=40)
    assert failed.returncode == 1 and "saved result can be replayed" in failed.stderr
    replay, status = decode(query(task=full_id, action="resume", read_only=True))
    assert replay and status["state"] == "awaiting-delivery"
    directory = tasks / f"pagos-query-{full_id}"
    segments = active_segments(directory)
    assert segments
    broken = segments[0]
    # Unreferenced crash debris is not part of a saved result. This positive
    # case also prevents the corruption fixture from silently picking it.
    orphan_id = hashlib.sha256(b"unreferenced corrupt segment fixture").hexdigest()
    orphan = directory / "objects" / orphan_id[:2] / orphan_id
    orphan.parent.mkdir(exist_ok=True)
    orphan.write_bytes(broken.read_bytes()[:-1])
    assert orphan not in active_segments(directory)
    snapshot = files(tasks)
    assert decode(query(task=full_id, action="resume", read_only=True))[0] == replay
    assert files(tasks) == snapshot
    saved = broken.read_bytes()
    broken.write_bytes(saved[:-1])
    corrupt_snapshot = files(tasks)
    query(task=full_id, action="resume", read_only=True, codes=(1,))
    assert files(tasks) == corrupt_snapshot
    broken.write_bytes(saved)
    restored_snapshot = files(tasks)
    replay2, status2 = decode(query(task=full_id, action="resume", read_only=True))
    assert replay == replay2 and status["work"] == status2["work"]
    assert files(tasks) == restored_snapshot
    assert cleanup(True)["reclaimed"] == 1

    # Pause cancellation is terminal for default maintenance.
    _, status = decode(query(slices=0))
    query(task=status["task"], action="cancel", codes=(0,))
    assert cleanup()["reclaimed"] == 1
    assert files(evidence) == immutable_evidence

    # Invalid combinations fail during option parsing without touching disk.
    invalid = root / "invalid"
    for extra in (["--evidence-channel=both"], ["--query-channel=value"],
                  ["--query-storage=paged"], ["--query-segment-bytes=75"],
                  ["--query-resident-bytes=100"], ["--evidence-root=aa"],
                  ["--evidence-mode=summaries"], ["--query-ack=1"],
                  ["--query-read-only"], ["--query-action=resume"]):
        query(directory=invalid, extra=extra, codes=(2,))
        assert not invalid.exists()
    query(task="0" * 64, action="resume", read_only=True,
          extra=["--recursive-max-steps=1"], directory=invalid, codes=(2,))
    assert not invalid.exists()
print("recursive task CLI: continuation, typed bindings, replay, receipts, output/storage failure and cleanup verified")
