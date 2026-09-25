"""Compile emitted LLVM IR and check its result and exact input read sequence."""

import pathlib
import subprocess
import sys
import tempfile


def main():
    pagosc, clang, source, expected, *inputs = sys.argv[1:]
    emitted = subprocess.run(
        [pagosc, "emit-llvm", source], capture_output=True, text=True, timeout=10
    )
    assert emitted.returncode == 0, emitted.stderr
    with tempfile.TemporaryDirectory(prefix="pagos-residual-") as directory:
        root = pathlib.Path(directory)
        ir = root / "program.ll"
        ir.write_text(emitted.stdout)
        executable = root / "program"
        subprocess.run(
            [clang, str(ir), str(pathlib.Path(__file__).with_name("runtime.c")),
             "-o", str(executable)],
            check=True,
        )
        executed = subprocess.run(
            [str(executable), *inputs], capture_output=True, text=True, timeout=5
        )
    lines = [f"input={value}" for value in inputs]
    if expected == "trap":
        assert executed.returncode < 0, executed
    else:
        assert executed.returncode == 0, executed
        lines.append(f"result={expected}")
    assert executed.stdout.splitlines() == lines, executed


if __name__ == "__main__":
    main()
