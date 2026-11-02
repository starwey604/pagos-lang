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
                   driver="clang", shared=False, import_library=None,
                   link_options=()):
    if driver == "msvc":
        if source is None:
            raise ValueError("MSVC C compilation requires a source file")
        command = [compiler, "/nologo", "/TC", "/std:c11", "/MT",
                   *(["/LD"] if shared else []),
                   "/D_CRT_SECURE_NO_WARNINGS", "/O2" if optimize else "/Od",
                   str(source),
                   "/Fo" + str(output.with_suffix(".obj")),
                   "/Fe" + str(output), "/link", *map(str, objects)]
        if import_library is not None:
            command.append("/IMPLIB:" + str(import_library))
        command.extend(link_options)
    else:
        # Resolve the caller's references before scanning static archives;
        # ELF linkers need this order even when a COFF linker rescans them.
        command = [compiler, "-O2" if optimize else "-O0",
                   *(["-shared"] if shared else []),
                   *(["-fPIC"] if shared and os.name != "nt" else []),
                   *([str(source)] if source is not None else []),
                   *map(str, objects), "-o", str(output)]
        for option in link_options:
            # A bare SDK .lib is a linker search name, not a source filename
            # for the Clang driver to resolve against the current directory.
            if os.name == "nt" and str(option).lower().endswith(".lib"):
                command.extend(["-Xlinker", str(option)])
            else:
                command.append(str(option))
        if import_library is not None:
            command.extend(["-Xlinker", "/IMPLIB:" + str(import_library)])
    subprocess.run(command, cwd=output.parent, check=True, timeout=30)
