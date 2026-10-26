"""C4d2 real compiler processes: symbolic summaries, identity and cold reads."""
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
    "fn known(n: u32) -> u32 { if n == 0 { 0 } else { known(n - 1) }; 7 } "
    "fn input(n: u32) -> u32 { if n == 0 { external_input() } else { input(n - 1) } } "
    "fn project(x: u32, y: u32) -> u32 { [x, y][0] } "
    "fn scale(k: u32, x: u32) -> u32 { k * x } "
    "fn wrapper(x: u32) -> u32 { input(x) } "
    "fn isolated(x: u32) -> u32 { runtime let value = sibling(x); value } "
)
SIBLING = "fn sibling(x: u32) -> u32 { let n0 = x + 1; " + " ".join(
    f"let n{i} = n{i-1} + 1;" for i in range(1, 257)) + " n256 } "
PROGRAM += SIBLING + (
    "let a = external_input(); let b = external_input(); "
    "let aa = [rotate(a, b), even(a), known(a), input(a), project(a, b), "
    "scale(4, a), scale(5, b), wrapper(a), wrapper(b), sibling(a), isolated(a)];"
)


def files(root):
    return {str(path.relative_to(root)): (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns)
            for path in root.rglob("*") if path.is_file()}


def stats(result):
    return {key: int(value) for key, value in re.findall(r"([a-z-]+)=(\d+)", result.stderr)}


def decoded(result):
    value = json.loads(result.stdout)
    assert value["schema"] == 1 and value["local_ids"]
    assert not value["continuation_available"]
    assert all(len(part) == 64 for part in value["evidence"].values())
    return value


with tempfile.TemporaryDirectory(prefix="pagos-recursive-cli-") as temporary:
    root = Path(temporary)
    source, store = root / "input.pgs", root / "evidence"
    source.write_text(PROGRAM)

    def run(*options, mode="recursive-summary", name=None, write=False, codes=(0,)):
        args = [BINARY, "--target=riscv32-unknown-elf", f"--evidence-mode={mode}",
                f"--evidence-{'write' if write else 'read'}={store}", "--provenance-query-stats"]
        if not write:
            args.append("--max-fuel=0")
        if name:
            args.append(f"--evidence-summary={name}")
        result = subprocess.run([*args, *options, "explain-provenance", str(source)],
                                text=True, capture_output=True, timeout=40)
        assert result.returncode in codes, (args, options, result.returncode, result.stdout, result.stderr)
        if result.returncode in (1, 2):
            assert not result.stdout, "failed query mixed partial JSON or ordinary text into stdout"
        return result

    published = decoded(run(mode="summaries", write=True))
    assert published["stage_runs"] == 1
    before = files(store)
    catalog_result = run(mode="summaries")
    catalog = decoded(catalog_result)
    assert catalog["stage_runs"] == 0 and stats(catalog_result)["cold-objects"] == 0
    assert catalog["summaries"] == published["summaries"]
    assert catalog["evidence"] == published["evidence"]
    scale = decoded(run(mode="summaries", name="scale"))["summaries"]
    assert len(scale) == 2 and scale[0]["key"] != scale[1]["key"]
    assert {entry["arguments"][0]["value"] for entry in scale} == {"4", "5"}
    assert all(entry["arguments"][1]["value"] is None for entry in scale)
    failure = run(name="scale", codes=(1,))
    assert "ambiguous" in failure.stderr
    run(name="absent", codes=(1,))
    run(f"--evidence-summary-key={'0' * 64}", codes=(1,))
    by_key = decoded(run(f"--evidence-summary-key={scale[0]['key']}"))
    assert by_key["summary"] == scale[0]
    # Slots address residual Runtime formals, not the original full argument
    # list: scale's Comptime k is absent, so x is formal slot zero.
    assert {atom["slot"] for atom in by_key["results"][0]["atoms"] if atom["kind"] == "parameter"} == {0}
    assert decoded(run(f"--evidence-summary-key={scale[0]['key']}", mode="summaries"))["summaries"] == [scale[0]]

    solved = {}
    for name in ("rotate", "even", "odd", "known", "input", "project", "wrapper"):
        large = decoded(run(name=name, *["--provenance-slice=100000"]))
        small = decoded(run(name=name, *["--provenance-slice=1", "--evidence-cache-bytes=0"]))
        assert large["stage_runs"] == small["stage_runs"] == 0
        assert not large["restart_required"] and len(large["results"]) == 2
        for a, b in zip(large["results"], small["results"]):
            a["work"].pop("slices")
            b["work"].pop("slices")
            assert a == b, (name, a, b)
            assert a["fixed_point"] and not a["concrete_sources_complete"]
            assert a["proof_available"] and a["issues"] == 0 and not a["gaps"]
            nodes = {node["id"]: node for node in a["nodes"]}
            assert len(nodes) == len(a["nodes"]) == a["work"]["nodes"]
            assert {node for component in a["components"] for node in component["nodes"]} == nodes.keys()
            assert len(a["links"]) == a["work"]["links"]
            assert all(link["from"] in nodes and link["to"] in nodes for link in a["links"])
            for atom in a["atoms"]:
                if atom["kind"] == "parameter":
                    assert atom["owner"] == large["summary"]["key"]
                else:
                    assert atom["node"] in nodes
            single = decoded(run(name=name, *[f"--evidence-channel={a['channel']}"]))
            assert len(single["results"]) == 1
            single["results"][0]["work"].pop("slices")
            assert single["results"][0] == a
        solved[name] = large

    rotate = solved["rotate"]["results"][0]
    assert rotate["precision"] == "exact-for-recorded-model" and not rotate["instances_merged"]
    assert {atom["slot"] for atom in rotate["atoms"]} == {0, 1}
    assert any(component["cyclic"] for component in rotate["components"])
    mutual = solved["even"]["results"][0]
    assert any(len(component["summaries"]) == 2 and component["cyclic"] for component in mutual["components"])
    assert solved["input"]["results"][0]["precision"] == "conservative"
    assert solved["input"]["results"][1]["precision"] == "conservative"
    assert any(node["decision"] and node["decision"]["recursive_completion"] and
               node["decision"]["stage"] == "Runtime"
               for node in solved["known"]["results"][0]["nodes"])
    project_value, project_effects = solved["project"]["results"]
    assert {atom["slot"] for atom in project_value["atoms"] if atom["kind"] == "parameter"} == {0}
    assert {atom["slot"] for atom in project_effects["atoms"] if atom["kind"] == "parameter"} == {0, 1}

    zero_result = run("--recursive-max-steps=0", name="rotate", codes=(3,))
    zero = decoded(zero_result)
    assert stats(zero_result)["cold-objects"] == 0 and zero["restart_required"]
    for result in zero["results"]:
        assert result["precision"] == "unresolved" and not result["fixed_point"]
        assert result["work"]["steps"] == 0 and not result["proof_available"]
        assert not result["nodes"] and not result["links"] and not result["components"]
    partial = decoded(run("--recursive-max-steps=7", name="rotate", codes=(3,)))
    assert all(result["work"]["steps"] == 7 and result["precision"] == "unresolved" for result in partial["results"])
    exact_steps = rotate["work"]["steps"]
    complete = decoded(run(f"--recursive-max-steps={exact_steps}", "--evidence-channel=value", name="rotate"))
    assert complete["results"][0]["fixed_point"]
    short = decoded(run(f"--recursive-max-steps={exact_steps - 1}", "--evidence-channel=value", name="rotate", codes=(3,)))
    assert short["restart_required"] and not short["results"][0]["fixed_point"]
    assert files(store) == before, "read-only queries wrote evidence or task state"

    # Whitespace reuses semantic evidence and keys, with fresh position/revision identity.
    shift = "// revised source positions\n\n"
    source.write_text(shift + PROGRAM)
    revised = decoded(run(mode="summaries"))
    assert revised["evidence"]["closure"] == catalog["evidence"]["closure"]
    assert revised["evidence"]["positions"] != catalog["evidence"]["positions"]
    assert revised["evidence"]["source_revision"] != catalog["evidence"]["source_revision"]
    assert revised["summaries"] == catalog["summaries"]
    moved = decoded(run(name="input"))
    for old, new in zip(solved["input"]["results"], moved["results"]):
        assert old["atoms"] == new["atoms"]
        assert all(b["site"]["begin"] == a["site"]["begin"] + len(shift)
                   for a, b in zip(old["nodes"], new["nodes"]))
    assert files(store) == before
    source.write_text(PROGRAM.replace("scale(5, b)", "scale(6, b)"))
    run(mode="summaries", codes=(1,))
    source.write_text(PROGRAM)
    run("--target=x86_64-unknown-linux-gnu", mode="summaries", codes=(1,))

    # An unreadable sibling is not loaded by a selected summary. Hot metadata
    # remains inspectable even after all cold objects disappear.
    details = [path for path in (store / "objects").rglob("*") if path.is_file() and path.read_bytes()[8] == 11]
    sibling = max(details, key=lambda path: path.stat().st_size)
    intact = sibling.read_bytes()
    sibling.write_bytes(intact[:-1] + bytes([intact[-1] ^ 1]))
    decoded(run(name="rotate"))
    run(name="sibling", codes=(1,))
    sibling.write_bytes(intact)
    sibling.unlink()
    decoded(run(name="rotate"))
    run(name="sibling", codes=(1,))
    # Value can finish while Effects needs missing initializer evidence; no
    # successful first-channel JSON may escape a failed two-channel request.
    decoded(run("--evidence-channel=value", name="isolated"))
    run(name="isolated", codes=(1,))
    for path in details:
        path.unlink(missing_ok=True)
    cold_missing = run(mode="summaries")
    assert decoded(cold_missing)["summaries"] == catalog["summaries"]
    assert stats(cold_missing)["cold-objects"] == 0
    run(name="input", codes=(1,))
    decoded(run("--recursive-max-steps=0", name="input", codes=(3,)))
    hot = next(path for path in (store / "objects").rglob("*") if path.is_file() and path.read_bytes()[8] == 10)
    hot.unlink()
    run(mode="summaries", codes=(1,))

    # Failed Stage publication must not emit ordinary provenance text as JSON.
    source.write_text("comptime let bad = external_input();")
    before_failed = files(store)
    failed = run(mode="summaries", write=True, codes=(1,))
    assert "E2001" in failed.stderr
    assert files(store) == before_failed
    source.write_text("let plain = 7;")
    assert decoded(run(mode="summaries", write=True))["summaries"] == []
    assert decoded(run(mode="summaries"))["summaries"] == []
    run(name="absent", codes=(1,))
print("recursive CLI: catalog, exact/merged relations, cycles, budgets, revisions and lazy failures verified")
