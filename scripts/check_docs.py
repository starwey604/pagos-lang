#!/usr/bin/env python3
"""Run dependency-free checks for the repository's Markdown sources."""

from __future__ import annotations

import re
import sys
from pathlib import Path
from urllib.parse import unquote


ROOT = Path(__file__).resolve().parents[1]
LINK = re.compile(r"!?\[[^]]*\]\(([^)]+)\)")


def markdown_files() -> list[Path]:
    """Return all maintained Markdown files in stable order."""
    candidates = [ROOT / "README.md", ROOT / "AGENTS.md"]
    candidates.extend(sorted((ROOT / "docs").rglob("*.md")))
    return [path for path in candidates if path.is_file()]


def local_link_target(raw_target: str) -> str | None:
    """Extract a local path from a Markdown inline-link target."""
    target = raw_target.strip()
    if target.startswith("<") and ">" in target:
        target = target[1 : target.index(">")]
    else:
        target = target.split(maxsplit=1)[0]

    lowered = target.lower()
    if not target or target.startswith("#"):
        return None
    if lowered.startswith(("http://", "https://", "mailto:")):
        return None
    return unquote(target.split("#", 1)[0])


def check_file(path: Path) -> list[str]:
    """Return human-readable failures for one Markdown file."""
    relative = path.relative_to(ROOT)
    data = path.read_bytes()
    errors: list[str] = []

    if not data.endswith(b"\n"):
        errors.append(f"{relative}: missing final newline")

    text = data.decode("utf-8")
    lines = text.splitlines()
    h1_count = 0
    in_fence = False

    for number, line in enumerate(lines, start=1):
        location = f"{relative}:{number}"
        if line.rstrip() != line:
            errors.append(f"{location}: trailing whitespace")
        if "\t" in line:
            errors.append(f"{location}: tab character; use spaces")

        if line.startswith("```"):
            in_fence = not in_fence
        elif not in_fence and line.startswith("# "):
            h1_count += 1

        if "PagOS" in line and not any(
            phrase in line for phrase in ("not `PagOS`", "never `PagOS`")
        ):
            errors.append(f"{location}: use the canonical spelling `Pagos`")

        if in_fence:
            continue
        for match in LINK.finditer(line):
            target = local_link_target(match.group(1))
            if target is None:
                continue
            resolved = (path.parent / target).resolve()
            try:
                resolved.relative_to(ROOT)
            except ValueError:
                errors.append(f"{location}: local link escapes repository: {target}")
                continue
            if not resolved.exists():
                errors.append(f"{location}: broken local link: {target}")

    if in_fence:
        errors.append(f"{relative}: unclosed fenced code block")
    if h1_count != 1:
        errors.append(f"{relative}: expected one level-one heading, found {h1_count}")
    return errors


def main() -> int:
    """Check all docs and return a process exit status."""
    files = markdown_files()
    failures = [error for path in files for error in check_file(path)]
    if failures:
        print("Documentation checks failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    print(f"Documentation checks passed ({len(files)} files).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
