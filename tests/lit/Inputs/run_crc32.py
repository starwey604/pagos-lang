"""Check every generated table entry and residual CRCs against Python zlib."""

import pathlib
import random
import re
import subprocess
import sys
import tempfile
from test_runtime import executable_path
import zlib


# Recursive HIR printing expands shared expressions (about 152 MB for this
# fixture). Allow sanitizer builds on hosted runners time to finish printing;
# generated programs retain their separate five-second execution deadline.
COMPILER_TIMEOUT = 60


def main():
    pagosc, clang, source = sys.argv[1:]
    # Only the folded check vector is needed, not an in-memory copy of the
    # entire expanded residual expression or a huge assertion failure dump.
    with tempfile.TemporaryFile(mode="w+") as hir:
        subprocess.run(
            [pagosc, "emit-hir", source], check=True, stdout=hir,
            stderr=subprocess.PIPE, text=True, timeout=COMPILER_TIMEOUT,
        )
        hir.seek(0)
        assert any(line.rstrip() == "binding known: u32 [Comptime] = 3421780262"
                   for line in hir), "missing or incorrect folded CRC check vector"
    emitted = subprocess.run(
        [pagosc, "emit-llvm", source], check=True, capture_output=True,
        text=True, timeout=COMPILER_TIMEOUT,
    ).stdout
    tables = re.findall(
        r"private unnamed_addr constant \[256 x i32\] \[([^\n]+)\]", emitted,
    )
    assert len(tables) == 1, "expected one deduplicated CRC table"
    actual = [int(value) & 0xFFFFFFFF
              for value in re.findall(r"i32 (-?\d+)", tables[0])]
    expected = [zlib.crc32(bytes([i]), 0xFFFFFFFF) ^ 0xFFFFFFFF
                for i in range(256)]
    assert actual == expected, "generated CRC table differs from zlib"

    rng = random.Random(0)
    messages = [list(b"123456789"), [0] * 9, [255] * 9,
                [0xFFFFFFFF, 256, 257, 511, 0x80000000, 1, 127, 128, 254]]
    messages.extend([rng.randrange(256) for _ in range(9)] for _ in range(12))
    with tempfile.TemporaryDirectory(prefix="pagos-crc32-") as directory:
        root = pathlib.Path(directory)
        ir = root / "program.ll"
        ir.write_text(emitted, encoding="utf-8", newline="\n")
        for optimization in ("-O0", "-O2"):
            executable = executable_path(root / "program")
            subprocess.run(
                [clang, optimization, str(ir),
                 str(pathlib.Path(__file__).with_name("runtime.c")),
                 "-o", str(executable)], check=True, timeout=30,
            )
            for values in messages:
                executed = subprocess.run(
                    [str(executable), *map(str, values)], check=True,
                    capture_output=True, text=True, timeout=5,
                )
                checksum = zlib.crc32(bytes(value & 255 for value in values))
                expected_lines = [f"input={value}" for value in values]
                expected_lines.append(f"result={checksum}")
                assert executed.stdout.splitlines() == expected_lines, executed


if __name__ == "__main__":
    main()
