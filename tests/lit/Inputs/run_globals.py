"""Verify actual static storage, relocations and independent module identity."""
from pathlib import Path
import subprocess
import sys
import tempfile
from test_runtime import executable_path


def main():
    pagosc, clang, *arguments = sys.argv[1:]
    optimize = arguments[0] == "--optimize"
    source = arguments[-1]
    inputs = Path(__file__).parent
    with tempfile.TemporaryDirectory(prefix="pagos-globals-") as temporary:
        root = Path(temporary)
        objects = []
        for index, fixture in enumerate((source, inputs / "globals-peer.pgs")):
            obj = root / f"module-{index}.o"
            if optimize:
                ir = subprocess.check_output([pagosc, "emit-llvm", str(fixture)], timeout=10)
                subprocess.run([clang, "-O2", "-x", "ir", "-c", "-", "-o", str(obj)],
                               input=ir, check=True, timeout=20)
            else:
                subprocess.run([pagosc, "emit-obj", str(fixture), "-o", str(obj)],
                               check=True, timeout=20)
            objects.append(str(obj))
        metadata = subprocess.check_output(
            ["llvm-readobj", "--sections", "--symbols", "--relocations", objects[0]], text=True)
        coff = Path(objects[0]).read_bytes().startswith(b"\x64\x86")
        for section in (".data", ".bss", ".rdata" if coff else ".rodata"):
            assert f"Name: {section}" in metadata, metadata
        for name in ("counter", "configured", "identity", "twin"):
            assert f"Name: pagos.global.{name}" in metadata, metadata
        local = "StorageClass: Static" if coff else "Binding: Local"
        relocation = "IMAGE_REL_AMD64_" if coff else "R_X86_64_"
        assert local in metadata and relocation in metadata, metadata
        program = executable_path(root / "program")
        subprocess.run([clang, "-O2", *objects, str(inputs / "globals.c"), "-o", str(program)],
                       check=True, timeout=20)
        subprocess.run([str(program)], check=True, timeout=5)


if __name__ == "__main__":
    main()
