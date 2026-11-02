#!/usr/bin/env python3
"""Exercise native Unicode paths without imposing an encoding on wire names."""

import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def inventory(directory):
    return {path.relative_to(directory):
            (hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_mtime_ns)
            for path in directory.rglob("*") if path.is_file()}


def counts(text):
    return {key: int(value) for key, value in re.findall(r"([a-z-]+)=(\d+)", text)}


def main():
    compiler = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="pagos-host-paths-") as temporary:
        root = Path(temporary) / "中文 空格 workspace"
        root.mkdir()
        # Also exercise Unicode in the executable path and current directory.
        tools = root / "编译 工具"
        tools.mkdir()
        executable = tools / ("编译器" + compiler.suffix)
        shutil.copy2(compiler, executable)
        source = root / "源码 目录" / "输入 文件.pgs"
        source.parent.mkdir()
        text = ("fn square(x: u32) -> u32 { x * x } "
                "let known = square(7); let aa = external_input() + known;\n")
        source.write_text(text, encoding="utf-8")

        def run(*args, expected=(0,)):
            result = subprocess.run([str(executable), *map(str, args)], cwd=root,
                                    capture_output=True, text=True,
                                    encoding="utf-8", timeout=30)
            assert result.returncode in expected, (
                args, result.returncode, result.stdout, result.stderr)
            return result

        target = ["--target=riscv32-unknown-elf"]

        def compile(command="check", *options, expected=(0,)):
            return run(*target, *options, command, source, expected=expected)

        compile()
        baseline = compile("emit-hir").stdout
        output = root / "对象 输出.o"
        for _ in range(2):
            compile("emit-obj", "-o", output)
            assert output.read_bytes().startswith(b"\x7fELF")
        original = output.read_bytes()
        sentinel = Path(str(output) + ".tmp")
        sentinel.write_bytes(b"keep temporary")
        failure = compile("emit-obj", "-o", output, expected=(1,))
        assert "cannot create object temporary file" in failure.stderr
        assert output.read_bytes() == original and sentinel.read_bytes() == b"keep temporary"
        sentinel.unlink()
        compile("emit-obj", "-o", source, expected=(1,))
        assert source.read_text(encoding="utf-8") == text
        output_directory = root / "输出 目录"
        output_directory.mkdir()
        (output_directory / "keep").write_bytes(b"keep directory")
        failed_replace = compile("emit-obj", "-o", output_directory, expected=(1,))
        assert "cannot replace object output" in failed_replace.stderr
        assert (output_directory / "keep").read_bytes() == b"keep directory"
        assert not Path(str(output_directory) + ".tmp").exists()

        cache = root / "语义 缓存"
        cache_options = [f"--cache-dir={cache}", "--cache-stats"]
        cold = compile("emit-hir", *cache_options, "--stage-cache=read-write")
        assert cold.stdout == baseline and counts(cold.stderr)["writes"] > 0
        before = inventory(cache)
        hot = compile("emit-hir", *cache_options, "--stage-cache=read-only")
        assert hot.stdout == baseline and counts(hot.stderr)["hits"] > 0
        assert inventory(cache) == before, "read-only cache modified files"
        # A bad semantic cache entry must still fall back to ordinary Stage.
        for damaged in (cache / "objects").glob("*/*"):
            damaged.write_bytes(b"corrupt")
        fallback = compile("emit-hir", *cache_options, "--stage-cache=read-only")
        assert fallback.stdout == baseline and counts(fallback.stderr)["hits"] == 0

        snapshot = root / "模块 快照"
        saved = compile("explain-provenance", f"--snapshot-write={snapshot}")
        before = inventory(snapshot)
        restored = compile("explain-provenance", f"--snapshot-read={snapshot}",
                           "--max-fuel=0")
        assert restored.stdout == saved.stdout and inventory(snapshot) == before
        for manifest in snapshot.glob("queries/**/*.manifest"):
            manifest.unlink()
        rejected = compile("explain-provenance", f"--snapshot-read={snapshot}",
                           "--max-fuel=0", expected=(1,))
        assert "snapshot load failed" in rejected.stderr and not rejected.stdout

        spill = root / "临时 证据"
        spilled = compile("emit-hir", f"--evidence-spill={spill}",
                          "--spill-block-bytes=128", "--spill-stats")
        assert spilled.stdout == baseline and counts(spilled.stderr)["written"] > 0
        assert not list(spill.glob("pagos-evidence-*")), "normal exit retained spill session"
        assert counts(run("reclaim-evidence", spill).stdout)["errors"] == 0

        evidence = root / "函数 证据"
        compile("explain-provenance", f"--evidence-write={evidence}", "--evidence-root=aa")
        tasks = root / "查询 任务"
        query_options = [f"--evidence-read={evidence}", "--evidence-root=aa",
                         f"--query-dir={tasks}", "--query-storage=paged",
                         "--provenance-slice=10000", "--max-fuel=0"]

        def query(*options, expected=(0, 3)):
            result = compile("explain-provenance", *query_options, *options, expected=expected)
            rows = [json.loads(line) for line in result.stdout.splitlines()]
            assert rows and rows[-1]["type"] == "status"
            assert all(row["schema"] == 1 for row in rows)
            assert rows[-1]["stage_runs"] == 0
            return rows

        rows = query("--query-action=start", "--query-max-slices=all")
        pages = [row for row in rows if row["type"] == "page"]
        assert pages and pages[0]["records"]
        task = rows[-1]["task"]
        before = inventory(tasks)
        resumed = query("--query-action=resume", f"--query-id={task}", "--query-read-only")
        assert [row for row in resumed if row["type"] == "page"] == pages
        assert resumed[-1]["generation"] == rows[-1]["generation"]
        assert resumed[-1]["work"] == rows[-1]["work"]
        assert inventory(tasks) == before, "read-only query modified task files"
        for _ in range(10):
            if rows[-1]["state"] == "completed":
                break
            pages = [row for row in rows if row["type"] == "page"]
            ack = [f"--query-ack={pages[0]['sequence']}"] if pages else []
            rows = query("--query-action=resume", f"--query-id={task}",
                         "--query-max-slices=all", *ack)
        assert rows[-1]["state"] == "completed"
        assert counts(run("reclaim-queries", tasks).stdout)["reclaimed"] == 1

        # File-as-directory failures are portable; they must be diagnostics,
        # cache fallback, or explicit-query failure rather than a crash.
        blocker = root / "错误 路径"
        blocker.write_text("keep", encoding="utf-8")
        invalid = blocker / "不存在.pgs"
        missing = run(*target, "check", invalid, expected=(1,))
        assert "could not open source file" in missing.stderr
        assert invalid.as_posix() in missing.stderr
        bad_output = compile("emit-obj", "-o", invalid, expected=(1,))
        assert "cannot create object temporary file" in bad_output.stderr
        fallback = compile("emit-hir", f"--cache-dir={invalid}", "--stage-cache=read-write")
        assert fallback.stdout == baseline
        compile("explain-provenance", f"--evidence-read={invalid}", expected=(1,))
        assert blocker.read_text(encoding="utf-8") == "keep"
        print("Unicode executable/cwd/source/output/cache/snapshot/spill/query paths and failures passed.")


if __name__ == "__main__":
    main()
