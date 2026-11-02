"""Link real C callers/callees against Pagos object output or optimized IR."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
from test_runtime import (assert_object_format, assert_trap, executable_path,
                          link_c_program)


def main():
    pagosc, clang, *arguments = sys.argv[1:]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-optimize", action="store_true")
    parser.add_argument("--optimize", action="store_true")
    parser.add_argument("--no-traps", action="store_true")
    parser.add_argument("--c-driver", choices=("clang", "msvc"), default="clang")
    parser.add_argument("--c-compiler")
    parser.add_argument("--archive-tool")
    parser.add_argument("source")
    parser.add_argument("extra", nargs="*")
    args = parser.parse_intermixed_args(arguments)
    if args.optimize and args.native_optimize:
        parser.error("choose external or native optimization, not both")
    source, extra = args.source, args.extra
    harness = (Path(extra[0]) if extra else Path(__file__).with_name("c_abi.c")).resolve()
    with tempfile.TemporaryDirectory(prefix="pagos-c-abi-") as directory:
        root = Path(directory)
        obj = root / "module.o"
        if args.optimize:
            ir = subprocess.check_output([pagosc, "emit-llvm", source], timeout=10)
            assert b"@pagos_main" not in ir
            subprocess.run([clang, "-O2", "-x", "ir", "-c", "-", "-o", str(obj)],
                           input=ir, check=True, timeout=20)
        else:
            subprocess.run([pagosc, *(["-O2"] if args.native_optimize else []), "emit-obj", source, "-o", str(obj)],
                           check=True, timeout=20)
        assert_object_format(obj)
        linked_object = obj
        if args.archive_tool:
            linked_object = root / "module.lib"
            subprocess.run([args.archive_tool, "rcs", str(linked_object), str(obj)],
                           check=True, timeout=20)
        program = executable_path(root / "program")
        link_c_program(args.c_compiler or clang, [linked_object], harness, program,
                       optimize=args.optimize or args.native_optimize,
                       driver=args.c_driver)
        subprocess.run([str(program)], check=True, timeout=5)
        trap_cases = extra[1:] or (["trap", "store-trap", "discard-trap"] if extra else ["trap"])
        if args.no_traps:
            trap_cases = []
        for case in trap_cases:
            trapped = subprocess.run([str(program), case], timeout=5)
            assert_trap(trapped)


if __name__ == "__main__":
    main()
