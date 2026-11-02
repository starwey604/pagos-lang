"""Run lit itself with Unicode, spaces and apostrophes in every project path."""
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import shlex
import shutil
import subprocess
import sys
import tempfile


compiler, checker, lit, python, clang, lld, nm, objdump, llvm_tools, source_root = sys.argv[1:]
sys.path.insert(0, source_root)
from pagos_test_format import shell_quote

# Check both path flavours on every host, including literal POSIX backslashes
# and Windows UNC roots. The internal shell must recover exactly one argument.
for path, expected in (
    (PurePosixPath("/tmp/输入 'file\\name.pgs"), "/tmp/输入 'file\\name.pgs"),
    (PureWindowsPath("C:\\输入 'files\\name.pgs"), "C:/输入 'files/name.pgs"),
    (PureWindowsPath(r"\\server\share\input.pgs"), "//server/share/input.pgs"),
):
    assert shlex.split(shell_quote(path)) == [expected], (path, shell_quote(path))

with tempfile.TemporaryDirectory(prefix="pagos-lit-paths-") as temporary:
    root = Path(temporary) / "中文 'lit paths"
    source = root / "源码 tests"
    inputs = source / "Inputs"
    inputs.mkdir(parents=True)
    tools = root / "工具 bin"
    tools.mkdir()
    binary = tools / Path(compiler).name
    filecheck = tools / Path(checker).name
    shutil.copy2(compiler, binary)
    shutil.copy2(checker, filecheck)
    original = Path(source_root)
    for name in ("lit.cfg.py", "pagos_test_format.py"):
        shutil.copy2(original / name, source / name)
    shutil.copy2(original / "Inputs/check_host_files.py", inputs / "check_host_files.py")
    fixture = """// RUN: %pagosc emit-hir %s > %t.first
// RUN: %pagosc emit-hir %s > %t.second
// RUN: %diff_text %t.first %t.second
// RUN: %FileCheck %s < %t.first
// RUN: %assert_absent %t.missing
// CHECK: binding result: u32 [Comptime] = 42
let result = 42;
"""
    (source / "输入 'file.pgs").write_text(fixture, encoding="utf-8", newline="\n")
    if os.name == "posix":
        # Also exercise source-path substitution and redirection through lit.
        (source / "反斜杠\\ 'file.pgs").write_text(
            fixture, encoding="utf-8", newline="\n")
    config = dict(pagosc=binary, filecheck=filecheck, python=python,
                  test_clang=clang, test_lld=lld, test_nm=nm, test_objdump=objdump,
                  llvm_tools_dir=llvm_tools, pagos_test_source_root=source,
                  pagos_test_exec_root=root / "执行 work")
    text = "\n".join(f"config.{name} = {str(value)!r}" for name, value in config.items())
    text += "\nlit_config.load_config(config, " + repr(str(source / "lit.cfg.py")) + ")\n"
    site = root / "configuration"
    site.mkdir()
    (site / "lit.site.cfg.py").write_text(text, encoding="utf-8", newline="\n")
    result = subprocess.run([lit, "-sv", "-j", "1", str(site)], capture_output=True,
                            encoding="utf-8", timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "Passed" in result.stdout, result.stdout
    print("lit: Unicode, spaces, apostrophes, executable paths, redirections and suffixes passed.")
