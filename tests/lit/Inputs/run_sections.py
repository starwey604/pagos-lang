"""Verify section names, ELF flags, alignment and native memory behavior."""

from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
from elf_storage import verify_storage


def main():
    pagosc, clang, source = sys.argv[1:]
    expected = {
        "rom": (".rodata.config", 32, False, False),
        "twin": (".rodata.config", 64, False, False),
        "seed": (".data.state", 64, True, False),
        "zero_data": (".data.zero", 16, True, False),
        "count": (".bss.state", 128, True, True),
        "natural": (".data.natural", 8, True, False),
        "page": (".bss.page", 4096, True, True),
    }
    with tempfile.TemporaryDirectory(prefix="pagos-sections-") as temporary:
        root = Path(temporary)
        for target in ("x86_64-unknown-linux-gnu", "riscv32-unknown-elf"):
            for optimize in (False, True):
                obj = root / f"{target}-{optimize}.o"
                if optimize:
                    ir = subprocess.check_output([pagosc, f"--target={target}", "emit-llvm", source], timeout=15)
                    subprocess.run([clang, f"--target={target}", "-O2", "-x", "ir", "-c", "-", "-o", str(obj)],
                                   input=ir, check=True, timeout=20)
                else:
                    subprocess.run([pagosc, f"--target={target}", "emit-obj", source, "-o", str(obj)],
                                   check=True, timeout=20)
                verify_storage(obj, expected)
                if target.startswith("x86_64"):
                    program = root / f"program-{optimize}"
                    subprocess.run([clang, "-O2", str(obj), str(Path(__file__).with_name("sections.c")),
                                    "-o", str(program)], check=True, timeout=20)
                    subprocess.run([str(program)], check=True, timeout=5)


if __name__ == "__main__":
    main()
