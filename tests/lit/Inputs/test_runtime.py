"""Capabilities shared by native-entry execution tests."""
import os
from pathlib import Path
import signal
import subprocess


def executable_path(path):
    return Path(str(path) + ".exe") if os.name == "nt" else Path(path)


def assert_trap(result):
    expected = 0xC000001D if os.name == "nt" else -signal.SIGILL
    actual = result.returncode & 0xFFFFFFFF if os.name == "nt" else result.returncode
    assert actual == expected, ("expected illegal-instruction trap", result)


def assert_object_format(path):
    expected = b"\x64\x86" if os.name == "nt" else b"\x7fELF"
    assert Path(path).read_bytes().startswith(expected), path


def link_c_program(compiler, objects, source, output, *, optimize=False,
                   driver="clang"):
    if driver == "msvc":
        command = [compiler, "/nologo", "/TC", "/std:c11", "/MT",
                   "/D_CRT_SECURE_NO_WARNINGS", "/O2" if optimize else "/Od",
                   str(source),
                   "/Fo" + str(output.with_suffix(".obj")),
                   "/Fe" + str(output), "/link", *map(str, objects)]
    else:
        # Resolve the caller's references before scanning static archives;
        # ELF linkers need this order even when a COFF linker rescans them.
        command = [compiler, "-O2" if optimize else "-O0", str(source),
                   *map(str, objects), "-o", str(output)]
    subprocess.run(command, cwd=output.parent, check=True, timeout=30)
