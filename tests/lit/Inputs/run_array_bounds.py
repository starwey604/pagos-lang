"""Execute cache-off and restored array reads at valid and invalid indices."""

import pathlib
import re
import shutil
import subprocess
import sys


def main():
    pagosc, clang, source, directory = sys.argv[1:]
    root = pathlib.Path(directory)
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    cache = root / "cache"
    programs = []
    outputs = []
    for mode in ("off", "read-write", "read-only"):
        compiled = subprocess.run(
            [pagosc, f"--stage-cache={mode}", f"--cache-dir={cache}",
             "--cache-stats", "emit-llvm", source],
            capture_output=True, text=True, check=True, timeout=15)
        if mode == "read-only":
            assert int(re.search(r"hits=(\d+)", compiled.stderr)[1]) == 1
        outputs.append(compiled.stdout)
        ir = root / f"{mode}.ll"
        ir.write_text(compiled.stdout)
        executable = root / mode
        subprocess.run([clang, str(ir), str(pathlib.Path(__file__).with_name(
            "runtime.c")), "-o", str(executable)], check=True)
        programs.append(executable)
    assert len(set(outputs)) == 1
    for index in (0, 3, 4, 4294967295):
        outcomes = []
        for executable in programs:
            result = subprocess.run([str(executable), str(index)],
                                    capture_output=True, text=True, timeout=5)
            outcomes.append((result.returncode, result.stdout, result.stderr))
            if index < 4:
                assert result.returncode == 0
                assert result.stdout.splitlines() == [
                    f"input={index}", f"result={(index + 1) ** 2 + 1}"]
            else:
                assert result.returncode < 0, "out-of-bounds read did not trap"
                assert result.stdout.splitlines() == [f"input={index}"]
        assert len(set(outcomes)) == 1


if __name__ == "__main__":
    main()
