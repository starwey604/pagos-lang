"""Portable filesystem assertions for lit's internal shell."""
import difflib
from pathlib import Path
import sys


mode, *arguments = sys.argv[1:]
if mode == "--absent":
    path = Path(arguments[0])
    assert not path.exists() and not path.is_symlink(), path
elif mode == "--equal":
    left, right = map(Path, arguments)
    # Windows CRT redirections can differ between LF and CRLF. Compare text
    # without weakening any non-newline character or wire-byte assertion.
    a = left.read_text(encoding="utf-8")
    b = right.read_text(encoding="utf-8")
    if a != b:
        sys.stderr.writelines(difflib.unified_diff(a.splitlines(True), b.splitlines(True),
                                                  str(left), str(right)))
        raise SystemExit(1)
else:
    raise SystemExit("unknown file assertion")
