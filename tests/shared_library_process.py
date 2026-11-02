"""Native shared C libraries: two-way interop, exports, loading and failures."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

INPUTS = Path(__file__).resolve().parent / "lit/Inputs"
sys.path.insert(0, str(INPUTS))
from test_runtime import assert_trap, executable_path, link_c_program


def execute(command, *, code=0, env=None):
    result = subprocess.run(list(map(str, command)), capture_output=True,
                            text=True, encoding="utf-8", errors="replace",
                            timeout=30, env=env)
    assert result.returncode == code, (command, result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pagosc")
    parser.add_argument("clang")
    parser.add_argument("readobj")
    parser.add_argument("--c-driver", choices=("clang", "msvc"), default="clang")
    parser.add_argument("--c-compiler")
    args = parser.parse_args()
    compiler = args.c_compiler or args.clang
    windows = os.name == "nt"
    system = ["kernel32.lib"] if windows else []
    rpath = [] if windows else ["-Wl,-rpath,$ORIGIN"]
    loader_flags = system if windows else ["-ldl"]
    with tempfile.TemporaryDirectory(prefix="pagos-shared-") as temporary:
        root = Path(temporary) / "共享 'libraries"
        binaries = root / "bin"
        binaries.mkdir(parents=True)
        loader = executable_path(binaries / "loader")
        link_c_program(compiler, [], INPUTS / "shared_loader.c", loader,
                       optimize=True, driver=args.c_driver,
                       link_options=loader_flags)
        for mode in ("legacy", "native", "external"):
            directory = root / mode
            directory.mkdir()
            peer = directory / ("pagos_shared_peer.dll" if windows else "libpagos_shared_peer.so")
            library = directory / ("pagos_shared.dll" if windows else "libpagos_shared.so")
            peer_import = peer.with_suffix(".lib") if windows else peer
            library_import = library.with_suffix(".lib") if windows else library
            peer_flags = system if windows else ["-Wl,-soname," + peer.name]
            link_c_program(compiler, [], INPUTS / "shared_peer.c", peer,
                           optimize=True, driver=args.c_driver, shared=True,
                           import_library=peer_import if windows else None,
                           link_options=peer_flags)
            obj = directory / ("library.obj" if windows else "library.o")
            source = INPUTS / "shared_library.pgs"
            policy = ["--shared-library"]
            if mode == "native":
                # Symbol/relocation policy must not leak into cached Stage
                # results or mutate a read-only cache shared with object mode.
                cache = directory / "cache"
                common = [f"--cache-dir={cache}"]
                execute([args.pagosc, *common, "--stage-cache=read-write", "emit-llvm", source])
                snapshot = {str(p.relative_to(cache)): (hashlib.sha256(p.read_bytes()).hexdigest(),
                                                       p.stat().st_mtime_ns)
                            for p in cache.rglob("*") if p.is_file()}
                cold = execute([args.pagosc, *policy, "emit-llvm", source]).stdout
                warm = execute([args.pagosc, *policy, *common, "--stage-cache=read-only",
                                "--cache-stats", "emit-llvm", source])
                assert warm.stdout == cold, warm
                hits = re.search(r"(?<![-a-z])hits=(\d+)", warm.stderr)
                assert hits and int(hits[1]) > 0, warm.stderr
                assert snapshot == {str(p.relative_to(cache)): (hashlib.sha256(p.read_bytes()).hexdigest(),
                                                               p.stat().st_mtime_ns)
                                    for p in cache.rglob("*") if p.is_file()}
            if mode == "external":
                ir = execute([args.pagosc, *policy, "emit-llvm", source]).stdout
                result = subprocess.run([args.clang, "-O2", *([] if windows else ["-fPIC"]),
                                         "-x", "ir", "-c", "-",
                                         "-o", str(obj)], input=ir, capture_output=True,
                                        text=True, encoding="utf-8", timeout=30)
                assert result.returncode == 0, result
            else:
                execute([args.pagosc, *policy, *(["-O2"] if mode == "native" else []),
                         "emit-obj", source, "-o", obj])
            # The SDK Clang link driver supplies native CRT startup for the
            # object-only Pagos library, independently of the C producer.
            flags = system if windows else [*rpath, "-Wl,-soname," + library.name]
            link_c_program(args.clang, [obj, peer_import], None, library,
                           shared=True, import_library=library_import if windows else None,
                           link_options=flags)
            metadata = execute([args.readobj, "--coff-exports" if windows else "--dyn-symbols",
                                library]).stdout
            exports = set(re.findall(r"Name: (shared_\w+)", metadata))
            assert exports == {"shared_apply", "shared_state", "shared_calls",
                               "shared_pid", "shared_signed", "shared_trap"}, metadata
            assert "pagos.global" not in metadata and "pagos.signed_helper" not in metadata, metadata
            dependencies = execute([args.readobj, "--coff-imports" if windows else "--needed-libs",
                                    library]).stdout
            assert peer.name in dependencies, dependencies
            if windows:
                sdk = execute([args.readobj, "--coff-imports", peer]).stdout
                assert "GetCurrentProcessId" in sdk, sdk
            consumer = executable_path(directory / "consumer")
            link_c_program(compiler, [library_import], INPUTS / "shared_consumer.c", consumer,
                           optimize=True, driver=args.c_driver, link_options=[*system, *rpath])
            execute([consumer])
            assert_trap(subprocess.run([str(consumer), "trap"], timeout=10))
            env = dict(os.environ, PAGOS_TEST_LIBRARY=str(library))
            execute([loader], env=env)
            execute([loader, "missing-symbol"], env=env, code=32)
            assert_trap(subprocess.run([str(loader), "trap"], env=env, timeout=10))
            absent = dict(env, PAGOS_TEST_LIBRARY=str(directory / "absent-library"))
            execute([loader], env=absent, code=31)
            # Load from a directory containing no dependency. The loader lives
            # outside it and the original library dir is never added to PATH.
            missing = directory / "missing dependency"
            missing.mkdir()
            isolated = missing / library.name
            shutil.copy2(library, isolated)
            execute([loader], env=dict(env, PAGOS_TEST_LIBRARY=str(isolated)), code=31)
    print("shared libraries: C imports/exports, private storage, load/unload, exact traps, "
          "missing libraries/dependencies/symbols passed in all three emission modes")


if __name__ == "__main__":
    main()
