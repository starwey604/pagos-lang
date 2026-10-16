"""Compare independent cache-off/cold/hot compilers and execute residual work."""

import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


def main():
    pagosc, clang, source, directory, expected, *inputs = sys.argv[1:]
    root = pathlib.Path(directory)
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True, exist_ok=True)

    def compile(mode, command, cache, extra=()):
        result = subprocess.run(
            [pagosc, f"--stage-cache={mode}", f"--cache-dir={cache}",
             "--cache-stats", *extra, command, source],
            capture_output=True, text=True, timeout=15,
        )
        telemetry = next(line for line in result.stderr.splitlines()
                         if line.startswith("stage-cache: "))
        stats = {key: int(value) for key, value in
                 re.findall(r"([a-z-]+)=(\d+)", telemetry)}
        diagnostics = result.stderr.replace(telemetry + "\n", "")
        return result, stats, diagnostics

    for command in ("check", "explain-stage", "explain-provenance", "emit-hir",
                    "emit-mir", "emit-llvm"):
        cache = root / command
        off, off_stats, off_errors = compile("off", command, cache)
        assert off.returncode == 0, off_errors
        assert not cache.exists(), "off mode wrote to disk"
        cold, cold_stats, cold_errors = compile("read-write", command, cache)
        assert cold_stats["writes"] > 0, cold_stats
        assert cold_stats["body-evaluations"] == off_stats["body-evaluations"]
        assert cold_stats["hits"] == 0, cold_stats
        before = {p.relative_to(cache): (p.read_bytes(), p.stat().st_mtime_ns)
                  for p in cache.rglob("*") if p.is_file()}
        hot, hot_stats, hot_errors = compile("read-only", command, cache)
        after = {p.relative_to(cache): (p.read_bytes(), p.stat().st_mtime_ns)
                 for p in cache.rglob("*") if p.is_file()}
        assert before == after, "read-only mode changed cache files"
        assert hot_stats["hits"] > 0, hot_stats
        assert hot_stats["bodies-skipped"] == hot_stats["hits"], hot_stats
        assert (hot_stats["body-evaluations"] + hot_stats["bodies-skipped"]
                == off_stats["body-evaluations"]), hot_stats
        assert (off.returncode, off.stdout, off_errors) == (
            cold.returncode, cold.stdout, cold_errors)
        assert (off.returncode, off.stdout, off_errors) == (
            hot.returncode, hot.stdout, hot_errors)

        if command == "explain-stage":
            fuel = int(re.search(r"analysis: fuel=(\d+)", off.stdout)[1])
            for extra in ([f"--max-fuel={n}"] for n in range(fuel + 1)):
                a, _, errors_a = compile("off", command, cache, extra)
                b, _, errors_b = compile("read-only", command, cache, extra)
                assert (a.returncode, a.stdout, errors_a) == (
                    b.returncode, b.stdout, errors_b), extra
            for option in ("--max-array-elements", "--max-array-bytes",
                           "--max-aggregate-members", "--max-aggregate-bytes"):
                name = {"--max-array-elements": "array-elements",
                        "--max-array-bytes": "array-bytes",
                        "--max-aggregate-members": "aggregate-members",
                        "--max-aggregate-bytes": "aggregate-bytes"}[option]
                amount = int(re.search(rf"{name}=(\d+)", off.stdout)[1])
                for n in sorted({0, max(0, amount - 1), amount, amount + 1}):
                    extra = [f"{option}={n}"]
                    a, _, errors_a = compile("off", command, cache, extra)
                    b, _, errors_b = compile("read-only", command, cache, extra)
                    assert (a.returncode, a.stdout, errors_a) == (
                        b.returncode, b.stdout, errors_b), extra
            for option in ("--max-recursion-depth", "--max-specializations", "--max-residual-specializations"):
                for n in range(4):
                    extra = [f"{option}={n}"]
                    a, _, errors_a = compile("off", command, cache, extra)
                    b, _, errors_b = compile("read-only", command, cache, extra)
                    assert (a.returncode, a.stdout, errors_a) == (
                        b.returncode, b.stdout, errors_b), extra

        if command == "emit-llvm":
            # The compiled hot output must execute every caller-side effect
            # once, even when a callee ignores the known argument value.
            ir = root / "program.ll"
            ir.write_text(hot.stdout)
            executable = root / "program"
            subprocess.run([clang, str(ir), str(pathlib.Path(__file__).with_name(
                "runtime.c")), "-o", str(executable)], check=True)
            executed = subprocess.run([str(executable), *inputs],
                                      capture_output=True, text=True, timeout=5)
            assert executed.returncode == 0, executed.stderr
            assert executed.stdout.splitlines() == [
                *[f"input={value}" for value in inputs], f"result={expected}"]

    # Comments/whitespace preserve semantic fingerprints and restore current
    # definition locations (including provenance and failure diagnostics).
    original = pathlib.Path(source).read_text()
    with tempfile.TemporaryDirectory(prefix="pagos-cache-source-") as work:
        changed = pathlib.Path(work) / "current.pgs"
        changed.write_text(original)
        source = str(changed)
        cache = root / "locations"
        compile("read-write", "explain-provenance", cache)
        changed.write_text("// shifted source\n\n" + original.replace("{", "{\n"))
        off, _, errors = compile("off", "explain-provenance", cache)
        hot, stats, hot_errors = compile("read-only", "explain-provenance", cache)
        assert stats["hits"] > 0, stats
        assert (off.returncode, off.stdout, errors) == (
            hot.returncode, hot.stdout, hot_errors)
        changed.write_text(changed.read_text() + "\nlet dependency = 123;\n")
        _, stats, _ = compile("read-only", "check", cache)
        assert stats["hits"] == 0, stats


if __name__ == "__main__":
    main()
