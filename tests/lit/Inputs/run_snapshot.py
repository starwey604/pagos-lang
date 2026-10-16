"""Cross-process snapshot restoration with current positions, no Stage replay."""
import pathlib
import subprocess
import sys
import tempfile


def main():
    pagosc, original = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pagos-snapshot-") as directory:
        directory = pathlib.Path(directory)
        source = directory / "module.pgs"
        source.write_text(pathlib.Path(original).read_text())
        cache = directory / "objects"

        def run(*options):
            return subprocess.run([pagosc, *options, "explain-provenance", str(source)],
                                  text=True, capture_output=True, timeout=15)

        off = run()
        assert off.returncode == 0, off.stderr
        exported = run(f"--snapshot-write={cache}")
        assert (exported.returncode, exported.stdout, exported.stderr) == (off.returncode, off.stdout, off.stderr)
        inventory = {p.relative_to(cache): (p.read_bytes(), p.stat().st_mtime_ns)
                     for p in cache.rglob("*") if p.is_file()}
        # Stage fuel zero demonstrates that load performs no Stage analysis.
        loaded = run(f"--snapshot-read={cache}", "--max-fuel=0")
        assert (loaded.returncode, loaded.stdout, loaded.stderr) == (0, off.stdout, off.stderr)
        source.write_text("// shifted revision\n\n" + source.read_text().replace("{", "{\n"))
        current = run()
        shifted = run(f"--snapshot-read={cache}", "--provenance-query=resumable", "--provenance-slice=1")
        assert (shifted.returncode, shifted.stdout, shifted.stderr) == (0, current.stdout, current.stderr)
        assert inventory == {p.relative_to(cache): (p.read_bytes(), p.stat().st_mtime_ns)
                             for p in cache.rglob("*") if p.is_file()}, "read rewrote snapshots"
        source.write_text(source.read_text() + "\nlet semantic_change = 123;\n")
        rejected = run(f"--snapshot-read={cache}")
        assert rejected.returncode == 1 and "snapshot load failed" in rejected.stderr


if __name__ == "__main__":
    main()
