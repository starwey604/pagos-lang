#!/usr/bin/env python3
"""Shared local/container CI entry; each invocation uses a fresh build tree."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preset", choices=("debug", "gcc-debug", "asan", "release"),
                        default="debug")
    parser.add_argument("--quality", action="store_true")
    parser.add_argument("--rv32", action="store_true")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    log_root = ROOT / "build/ci-logs" / args.preset
    log_root.mkdir(parents=True, exist_ok=True)
    versions = {}
    for tool in ("clang", "g++", "llvm-config", "cmake", "ninja", "lit",
                 "FileCheck", "clang-format", "clang-tidy", "python3"):
        versions[tool] = subprocess.check_output([tool, "--version"], text=True).strip()
    if versions["llvm-config"] != "22.1.8":
        raise SystemExit("CI baseline requires LLVM 22.1.8; review version updates explicitly")
    (log_root / "versions.json").write_text(json.dumps(versions, indent=2) + "\n")
    sequence = 0

    def run(command):
        nonlocal sequence
        sequence += 1
        print("+", " ".join(map(str, command)), flush=True)
        with (log_root / f"{sequence:02d}.log").open("w") as log:
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            print((log_root / f"{sequence:02d}.log").read_text(), flush=True)
            raise SystemExit(result.returncode)

    # Unique directory means an earlier successful build cannot hide a missing
    # dependency. Keep command logs outside the automatically removed tree.
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ci-", dir=ROOT / "build") as temporary:
        build = Path(temporary)
        run(["cmake", "--preset", args.preset, "-B", str(build)])
        run(["cmake", "--build", str(build), "-j", str(args.jobs)])
        run(["ctest", "--test-dir", str(build), "--output-on-failure", "--timeout", "300"])
        for source in sorted((ROOT / "benchmarks").glob("*.pgs")):
            run([str(build / "pagosc"), "check", str(source)])
        if args.quality:
            sources = sorted(str(p) for folder in ("include", "lib", "tools", "tests/unit")
                             for p in (ROOT / folder).rglob("*") if p.suffix in (".cpp", ".h"))
            run(["clang-format", "--dry-run", "--Werror", *sources,
                 str(ROOT / "platforms/qemu-rv32-virt/smoke.c"),
                 str(ROOT / "tests/lit/Inputs/c_abi.c"),
                 str(ROOT / "tests/lit/Inputs/pointers.c")])
            for folder in ("lib", "tools"):
                for source in sorted((ROOT / folder).rglob("*.cpp")):
                    run(["clang-tidy", "--quiet", "--warnings-as-errors=*",
                         "-p", str(build), str(source)])
            run(["python3", "scripts/check_docs.py"])
            run(["git", "diff", "--check"])
            run(["git", "show", "--format=", "--check", "HEAD"])
        if args.rv32:
            run(["python3", "scripts/check_rv32.py", "--pagosc", str(build / "pagosc")])
    print(f"PASS {args.preset}; logs: {log_root}", flush=True)


if __name__ == "__main__":
    main()
