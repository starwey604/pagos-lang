"""Main pagosc task CLI: durable-before-output pages and explicit receipts."""
import collections
import hashlib
import fcntl
import json
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

BINARY = sys.argv[1]


def run(args, codes=(0, 3)):
    result = subprocess.run([BINARY, *map(str, args)], capture_output=True,
                            text=True, timeout=30)
    assert result.returncode in codes, (args, result.returncode, result.stdout, result.stderr)
    return result


def files(root):
    return {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob("*") if path.is_file()}


def decode(result):
    records = [json.loads(line) for line in result.stdout.splitlines()]
    assert records and records[-1]["type"] == "status"
    assert all(row["schema"] == 1 for row in records)
    pages = [row for row in records if row["type"] == "page"]
    assert len(pages) <= 1
    return pages, records[-1]


class Fixture:
    def __init__(self, parent, text):
        self.source = parent / "input.pgs"
        parent.mkdir(parents=True)
        self.source.write_text(text)
        self.evidence = parent / "evidence"
        self.tasks = parent / "tasks"
        self.base = ["--target=riscv32-unknown-elf"]
        result = run([*self.base, f"--evidence-write={self.evidence}",
                      "--evidence-root=aa", "explain-provenance", self.source], (0,))
        assert "binding aa" in result.stdout
        self.semantic_files = files(self.evidence)

    def query(self, mode="sources", storage="memory", channel="value", task=None,
              action="start", extra=(), slices="all", size=16, read_only=False,
              codes=(0, 3), root="aa", directory=None):
        args = [*self.base, "--max-fuel=0", f"--evidence-read={self.evidence}", f"--evidence-root={root}",
                f"--evidence-mode={mode}", f"--query-dir={directory or self.tasks}",
                f"--query-storage={storage}", f"--query-action={action}", f"--provenance-slice={size}"]
        if mode != "topology":
            args += [f"--query-channel={channel}"]
        if task:
            args += [f"--query-id={task}"]
        if read_only:
            args += ["--query-read-only"]
        elif slices is not None:
            args += [f"--query-max-slices={slices}"]
        return run([*args, *extra, "explain-provenance", self.source], codes)

    def cleanup(self, discard=False):
        args = ["reclaim-queries", self.tasks]
        if discard:
            args += ["--discard-resumable"]
        result = run(args, (0,))
        return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", result.stdout)}

    def finish(self, mode, storage, channel="value", size=16, extra=(), replay=False):
        first = self.query(mode, storage, channel, size=size, extra=extra)
        pages, status = decode(first)
        task = status["task"]
        assert status["stage_runs"] == 0 and status["offloaded"]
        received = {}
        for step in range(200):
            if pages:
                page = pages[0]
                assert page["task"] == task and page["sequence"] not in received
                assert page["records"]
                received[page["sequence"]] = page
                if replay:
                    before = files(self.tasks)
                    ro = self.query(mode, storage, channel, task, "resume", extra=extra, read_only=True)
                    ro_pages, ro_status = decode(ro)
                    assert ro_pages == pages
                    assert ro_status["generation"] == status["generation"]
                    assert ro_status["work"] == status["work"]
                    assert files(self.tasks) == before, "read-only replay wrote files"
                    repeated = self.query(mode, storage, channel, task, "resume", extra=extra, size=size)
                    repeated_pages, repeated_status = decode(repeated)
                    assert repeated_pages == pages
                    assert repeated_status["work"] == status["work"], "unconfirmed replay charged work"
                acknowledgement = [f"--query-ack={page['sequence']}"]
            else:
                acknowledgement = []
            if status["state"] == "completed":
                break
            result = self.query(mode, storage, channel, task, "resume", extra=(*extra, *acknowledgement), size=size)
            next_pages, next_status = decode(result)
            assert next_status["stage_runs"] == 0 and next_status["offloaded"]
            for key, value in status["work"].items():
                assert next_status["work"][key] >= value
            pages, status = next_pages, next_status
        else:
            raise AssertionError("CLI task did not finish")
        assert sorted(received) == list(range(1, len(received) + 1))
        assert files(self.evidence) == self.semantic_files
        return [record for page in received.values() for record in page["records"]], status


def counter(records):
    return collections.Counter(json.dumps(record, sort_keys=True) for record in records)


PROGRAM = (
    "fn inner(x: u32) -> u32 { x + external_input() } "
    "fn outer(x: u32) -> u32 { inner(x) } "
    "fn recur(x: u32) -> u32 { if x == 0 { x } else { recur(x - 1) } } "
    "let a = external_input(); let b = external_input(); "
    "let aa = [outer(a), outer(b), outer(a), outer(b)]; let rr = recur(a); let known = 7;"
)
with tempfile.TemporaryDirectory(prefix="pagos-query-cli-") as temporary:
    parent = Path(temporary)
    f = Fixture(parent / "small", PROGRAM)
    for mode, channel in (("sources", "value"), ("sources", "effects"), ("witness", "value"), ("topology", "value")):
        oracle, reference = f.finish(mode, "memory", channel, size=100000)
        for storage in (("memory",) if mode == "topology" else ("memory", "paged")):
            actual, completed = f.finish(mode, storage, channel, replay=True)
            assert counter(actual) == counter(oracle), (mode, channel, storage)
            assert {k: v for k, v in completed["work"].items() if k != "slices"} == {
                k: v for k, v in reference["work"].items() if k != "slices"}
            if mode in ("witness", "topology"):
                assert actual == oracle, "ordered topology/witness records changed"
        if mode == "topology":
            assert completed["structural"] and not completed["sources_solved"]
        elif mode == "witness":
            assert completed["coverage"]["witness_found"]
            assert not completed["coverage"]["all_sources"]
        assert f.cleanup()["reclaimed"] >= 2
    _, known = decode(f.query(root="known", size=100000, codes=(0,)))
    assert known["coverage"]["precision"] == "exact-for-recorded-model"
    assert known["coverage"]["all_sources"]
    assert f.cleanup()["reclaimed"] == 1
    pages, recursive = decode(f.query(root="rr", size=100000))
    assert recursive["coverage"]["issues"] & 4
    assert recursive["coverage"]["precision"] == "conservative"
    if pages:
        _, recursive = decode(f.query(root="rr", task=recursive["task"], action="resume", slices=0,
            extra=(f"--query-ack={pages[0]['sequence']}",)))
    assert recursive["state"] == "completed" and recursive["coverage"]["issues"] & 4
    assert f.cleanup()["reclaimed"] == 1
    # Explicit zero-work pause, read-only inspection, cancellation and safe cleanup.
    _, paused = decode(f.query(slices=0))
    assert not any(paused["work"].values())
    before = files(f.tasks)
    pages, inspected = decode(f.query(task=paused["task"], action="resume", read_only=True))
    assert not pages and inspected["work"] == paused["work"]
    assert files(f.tasks) == before
    assert f.cleanup()["skipped"] == 1
    _, cancelled = decode(f.query(task=paused["task"], action="cancel"))
    assert cancelled["state"] == "cancelled"
    assert f.cleanup()["reclaimed"] == 1
    # Resume must not create a missing namespace; invalid targets cannot write.
    missing = parent / "missing"
    result = f.query(task="f" * 64, action="resume", directory=missing, codes=(1,))
    assert "not found" in result.stderr and not missing.exists()
    f.query(root="not_here", directory=missing, codes=(1,))
    assert not missing.exists()
    _, paused = decode(f.query(storage="paged", slices=0))
    before = files(f.tasks)
    for mode, storage, channel, root in (("witness", "paged", "value", "aa"),
                                        ("sources", "paged", "effects", "aa"),
                                        ("sources", "paged", "value", "rr"),
                                        ("sources", "memory", "value", "aa")):
        f.query(mode, storage, channel, paused["task"], "resume", root=root, codes=(1,))
        assert files(f.tasks) == before
    f.source.write_text(PROGRAM + " ")
    result = f.query(storage="paged", task=paused["task"], action="resume", codes=(1,))
    assert "binding mismatch" in result.stderr and not result.stdout
    assert files(f.tasks) == before
    f.source.write_text(PROGRAM)
    f.query(storage="paged", task=paused["task"], action="resume", extra=("--query-ack=999",), codes=(1,))
    assert files(f.tasks) == before
    f.query(storage="paged", task=paused["task"], codes=(1,))
    assert files(f.tasks) == before
    assert f.cleanup(True)["reclaimed"] == 1
    # Quota failure emits no uncommitted page; the initial head can be resumed.
    task = "1" * 64
    failed = f.query(storage="paged", task=task, extra=("--query-disk-bytes=1024",), codes=(1,))
    assert not failed.stdout and "limit" in failed.stderr
    result = f.query(storage="paged", task=task, action="resume", extra=("--query-disk-bytes=1048576",))
    assert decode(result)[1]["stage_runs"] == 0
    assert f.cleanup(True)["reclaimed"] == 1
    limited = "2" * 64
    result = f.query(storage="paged", task=limited, extra=("--query-max-records=1",), codes=(1,))
    assert not result.stdout and "limit" in result.stderr
    resumed = f.query(storage="paged", task=limited, action="resume", extra=("--query-max-records=4096",))
    assert decode(resumed)[1]["stage_runs"] == 0
    assert f.cleanup(True)["reclaimed"] == 1
    # A complete traversal with an unconfirmed page remains protected by default.
    pages, pending = decode(f.query(size=100000))
    assert pages and pending["state"] == "awaiting-delivery"
    before = files(f.tasks)
    f.query(task=pending["task"], action="cancel", codes=(1,))
    assert files(f.tasks) == before
    assert f.cleanup()["skipped"] == 1
    _, confirmed = decode(f.query(task=pending["task"], action="resume", slices=0,
                                  extra=(f"--query-ack={pages[0]['sequence']}",)))
    assert confirmed["state"] == "completed"
    assert f.cleanup()["reclaimed"] == 1
    # Missing state is reported without falling back to Stage.
    _, paused = decode(f.query(slices=0))
    leaves = [path for path in f.tasks.rglob("objects/*/*") if path.is_file() and path.read_bytes()[8] == 13]
    assert leaves
    leaves[0].unlink()
    result = f.query(task=paused["task"], action="resume", codes=(1,))
    assert "missing" in result.stderr and not result.stdout
    assert f.cleanup(True)["reclaimed"] == 1
    # New logical tasks get fresh IDs without a caller-supplied identifier.
    _, a = decode(f.query(slices=0))
    _, b = decode(f.query(slices=0))
    assert a["task"] != b["task"]
    assert f.cleanup(True)["reclaimed"] == 2
    assert files(f.evidence) == f.semantic_files
    # Low-cache paging exercises real on-disk query tables through the CLI.
    text = " ".join(f"let a{i} = external_input();" for i in range(257))
    text += " let aa = [" + ", ".join(f"a{i}" for i in range(257)) + "];"
    wide = Fixture(parent / "wide", text)
    expected, baseline = wide.finish("sources", "memory", size=100000)
    actual, completed = wide.finish("sources", "paged", size=128,
        extra=("--query-segment-bytes=128", "--query-resident-bytes=2048", "--query-cache-bytes=512"))
    assert counter(actual) == counter(expected)
    assert completed["work"]["admitted_states"] == baseline["work"]["admitted_states"]
    assert wide.cleanup()["reclaimed"] == 2
    # The task lease survives offload while stdout is blocked in a consumer.
    with tempfile.TemporaryFile() as stderr:
        args = [BINARY, *wide.base, "--max-fuel=0", f"--evidence-read={wide.evidence}",
                "--evidence-root=aa", f"--query-dir={wide.tasks}", "--query-max-slices=all",
                "--provenance-slice=100000", "explain-provenance", str(wide.source)]
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=stderr)
        try:
            fcntl.fcntl(process.stdout.fileno(), fcntl.F_SETPIPE_SZ, 4096)
            deadline = time.monotonic() + 30
            while not list(wide.tasks.glob("pagos-query-*/head")):
                assert process.poll() is None
                assert time.monotonic() < deadline
                time.sleep(0.01)
            assert process.poll() is None
            assert wide.cleanup(True)["active"] == 1
            process.kill()
            process.communicate(timeout=10)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=10)
        assert wide.cleanup()["skipped"] == 1
        assert wide.cleanup(True)["reclaimed"] == 1
    # A concurrent cancel is observed between slices by a running --all worker.
    large_text = " ".join(f"let a{i} = external_input();" for i in range(4097))
    large_text += " let aa = [" + ", ".join(f"a{i}" for i in range(4097)) + "];"
    running = Fixture(parent / "running", large_text)
    identifier = "3" * 64
    _, saved = decode(running.query(task=identifier, slices=0))
    with tempfile.TemporaryFile() as out:
        args = [BINARY, *running.base, "--max-fuel=0", f"--evidence-read={running.evidence}",
                "--evidence-root=aa", f"--query-dir={running.tasks}", f"--query-id={identifier}",
                "--query-action=resume", "--query-max-slices=all", "--provenance-slice=1",
                "explain-provenance", str(running.source)]
        process = subprocess.Popen(args, stdout=out, stderr=subprocess.PIPE, text=True)
        try:
            ready = process.stderr.readline()
            assert "loaded id=" in ready, ready
            process.send_signal(signal.SIGSTOP)
            _, cancelled = decode(running.query(task=identifier, action="cancel"))
            assert cancelled["state"] == "cancelled"
            process.send_signal(signal.SIGCONT)
            _, diagnostic = process.communicate(timeout=30)
            assert process.returncode == 1 and "cancelled by another writer" in diagnostic
            out.seek(0)
            assert not out.read(), "superseded worker emitted an uncommitted page"
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=10)
    assert running.cleanup()["reclaimed"] == 1
    assert files(running.evidence) == running.semantic_files
    # Starting during an explicit evidence publication runs Stage once; resume never does.
    fresh = parent / "write-start"
    fresh.mkdir()
    src = fresh / "input.pgs"
    src.write_text(PROGRAM)
    ev, tasks = fresh / "evidence", fresh / "tasks"
    result = run([*f.base, f"--evidence-write={ev}", "--evidence-root=aa",
                  f"--query-dir={tasks}", "--query-max-slices=0", "explain-provenance", src])
    _, started = decode(result)
    assert started["stage_runs"] == 1
    result = run([*f.base, "--max-fuel=0", f"--evidence-read={ev}", "--evidence-root=aa",
                  f"--query-dir={tasks}", f"--query-id={started['task']}", "--query-action=resume",
                  "--query-max-slices=0", "explain-provenance", src])
    assert decode(result)[1]["stage_runs"] == 0
    # A failed Stage run cannot mix ordinary text into JSON or create task files.
    bad = parent / "bad.pgs"
    bad.write_text("comptime let aa = external_input();")
    bad_evidence, bad_tasks = parent / "bad-evidence", parent / "bad-tasks"
    failed = run([*f.base, f"--evidence-write={bad_evidence}", "--evidence-root=aa",
                  f"--query-dir={bad_tasks}", "explain-provenance", bad], (1,))
    assert not failed.stdout
    assert not bad_tasks.exists() and not bad_evidence.exists()
print("query CLI: persisted pages, receipts, read-only access, bindings, quota and cleanup verified")
