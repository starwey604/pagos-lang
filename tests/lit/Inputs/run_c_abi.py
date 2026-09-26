"""Link real C callers/callees against Pagos object output or optimized IR."""

from pathlib import Path
import signal
import subprocess
import sys
import tempfile


def main():
    pagosc, clang, *arguments = sys.argv[1:]
    optimize = arguments[0] == "--optimize"
    if optimize:
        arguments = arguments[1:]
    source, = arguments
    with tempfile.TemporaryDirectory(prefix="pagos-c-abi-") as directory:
        root = Path(directory)
        obj = root / "module.o"
        if optimize:
            ir = subprocess.check_output([pagosc, "emit-llvm", source], timeout=10)
            assert b"@pagos_main" not in ir
            subprocess.run([clang, "-O2", "-x", "ir", "-c", "-", "-o", str(obj)],
                           input=ir, check=True, timeout=20)
        else:
            subprocess.run([pagosc, "emit-obj", source, "-o", str(obj)],
                           check=True, timeout=20)
            assert obj.read_bytes().startswith(b"\x7fELF")
        program = root / "program"
        subprocess.run([clang, "-O2" if optimize else "-O0", str(obj),
                        str(Path(__file__).with_name("c_abi.c")), "-o", str(program)],
                       check=True, timeout=20)
        subprocess.run([str(program)], check=True, timeout=5)
        trapped = subprocess.run([str(program), "trap"], timeout=5)
        assert trapped.returncode == -signal.SIGILL, trapped


if __name__ == "__main__":
    main()
