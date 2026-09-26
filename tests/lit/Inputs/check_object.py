"""Check object output safety, the legacy entry, and separate-module linking."""

from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    pagosc, clang, fixture = sys.argv[1:]

    def invoke(*arguments, error=None):
        result = subprocess.run([pagosc, *map(str, arguments)],
                                capture_output=True, text=True, timeout=20)
        if error is None:
            assert result.returncode == 0, result
            assert not result.stdout, result
        else:
            assert result.returncode != 0 and error in result.stderr, result

    with tempfile.TemporaryDirectory(prefix="pagos-objects-") as directory:
        root = Path(directory)
        source = root / "source.pgs"
        text = Path(fixture).read_text()
        source.write_text(text)
        obj = root / "program.o"
        marker = b"previous output\n"
        obj.write_bytes(marker)
        invoke("emit-obj", source, error="emit-obj requires -o")
        invoke("check", source, "-o", obj, error="other commands do not accept -o")
        invoke("emit-obj", source, "-o", error="exactly one output path")
        invoke("emit-obj", source, "-o", obj, "-o", obj,
               error="exactly one output path")
        invoke("emit-obj", source, "-o", "--target=oops",
               error="invalid object output path")
        for alias in (source, root / "hardlink.pgs", root / "symlink.pgs"):
            if alias.name == "hardlink.pgs":
                alias.hardlink_to(source)
            elif alias.name == "symlink.pgs":
                alias.symlink_to(source)
            invoke("emit-obj", source, "-o", alias,
                   error="must not replace the source file")
            assert source.read_text() == text
        temporary = Path(str(obj) + ".tmp")
        temporary.write_bytes(b"unrelated temporary")
        invoke("emit-obj", source, "-o", obj, error="cannot create object temporary")
        assert temporary.read_bytes() == b"unrelated temporary"
        assert obj.read_bytes() == marker
        temporary.unlink()
        invalid = root / "invalid.pgs"
        invalid.write_text("static let x = external_input();")
        invoke("emit-obj", invalid, "-o", obj, error="E2001")
        assert obj.read_bytes() == marker and not temporary.exists()
        invoke("emit-obj", source, "-o", root, error="cannot replace object output")
        assert not Path(str(root) + ".tmp").exists()
        invoke("emit-obj", source, "-o", root / "missing/file.o",
               error="cannot create object temporary")
        invoke("emit-obj", source, "-o", obj)
        assert obj.read_bytes().startswith(b"\x7fELF") and not temporary.exists()
        caller = root / "caller.c"
        caller.write_text("extern unsigned pagos_main(void); "
                          "int main(void) { return pagos_main() != 42; }")
        program = root / "program"
        subprocess.run([clang, str(obj), str(caller), "-o", str(program)],
                       check=True, timeout=20)
        subprocess.run([str(program)], check=True, timeout=5)
        # Separately compiled Pagos modules export exactly their source names,
        # without duplicate synthetic entry symbols or implicit initialization.
        source.write_text("export fn first(x: u32) -> u32 { x + 1 }")
        second = root / "second.pgs"
        second.write_text("extern fn first(x: u32) -> u32; "
                          "export fn second(x: u32) -> u32 { first(x) + 1 }")
        second_obj = root / "second.o"
        invoke("emit-obj", source, "-o", obj)
        invoke("emit-obj", second, "-o", second_obj)
        caller.write_text("extern unsigned second(unsigned); "
                          "int main(void) { return second(40) != 42; }")
        subprocess.run([clang, str(obj), str(second_obj), str(caller),
                        "-o", str(program)], check=True, timeout=20)
        subprocess.run([str(program)], check=True, timeout=5)


if __name__ == "__main__":
    main()
