#!/usr/bin/env python3
"""Check stable Stage cache identities in separate executable processes."""

from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    probe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="pagos-cache-identity-") as temporary:
        directory = Path(temporary)
        source = directory / "input.pgs"
        plain = "fn square(x: u32) -> u32 { x * x } let result = square(7);\n"
        source.write_text(plain)

        def run(bits="32", cwd=None):
            output = subprocess.check_output([probe, str(source), bits], text=True, cwd=cwd)
            values = output.splitlines()
            assert len(values) == 2 and all(len(value) == 64 for value in values)
            return values

        baseline = run()
        assert run() == baseline, "fresh process changed stable hashes"
        assert run(cwd=directory) == baseline, "explicit module identity depends on cwd"
        source.write_text("// offset changes\n\nfn square( x: u32 ) -> u32 {\n x * x\n}\nlet result = square(7);\n")
        assert run() == baseline, "whitespace/comments changed semantic hashes"
        source.write_text(plain.replace("x * x", "x + x"))
        assert all(a != b for a, b in zip(run(), baseline)), "body edit reused identity"
        source.write_text(plain + "fn unrelated() -> u32 { 9 }\n")
        edited = run()
        assert edited[0] != baseline[0] and edited[1] == baseline[1]
        source.write_text(plain)
        assert run("64") != baseline, "target change reused query key"
        print("Cross-process identity, source edits, explicit module and target checks passed.")


if __name__ == "__main__":
    main()
