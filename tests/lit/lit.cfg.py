import errno
import os
import platform
import tempfile

import lit.formats


import sys
sys.path.insert(0, config.pagos_test_source_root)
from pagos_test_format import PagosShTest, shell_quote


config.name = "Pagos"
config.test_format = PagosShTest()
config.suffixes = [".pgs"]
config.excludes = ["Inputs"]
config.test_source_root = config.pagos_test_source_root
config.test_exec_root = config.pagos_test_exec_root
# Frontend/IR goldens specify a stable semantic target. Runtime helpers receive
# the raw executable and select their real host or an explicit cross target.
config.substitutions.append(("%pagosc_host", shell_quote(config.pagosc)))
config.substitutions.append(("%pagosc", shell_quote(config.pagosc) +
                              " --target=x86_64-unknown-linux-gnu"))
config.substitutions.append(("%FileCheck", shell_quote(config.filecheck)))
for token, mode in (("%assert_absent", "--absent"), ("%diff_text", "--equal")):
    config.substitutions.append((token, " ".join(map(shell_quote, (
        config.python, os.path.join(config.pagos_test_source_root, "Inputs", "check_host_files.py"), mode)))))
config.environment["PYTHONUTF8"] = "1"
config.environment["PYTHONIOENCODING"] = "utf-8"
config.available_features.add("host-" + platform.system().lower())
if platform.machine().lower() in ("amd64", "x86_64"):
    if platform.system() in ("Windows", "Linux"):
        config.available_features.add("native-entry")
        config.available_features.add("minimal-c-abi")
    if platform.system() == "Linux":
        config.available_features.add("elf-native")
os.makedirs(config.test_exec_root, exist_ok=True)
with tempfile.TemporaryDirectory(dir=config.test_exec_root) as directory:
    target = os.path.join(directory, "target")
    with open(target, "w"):
        pass
    try:
        os.symlink(target, os.path.join(directory, "link"))
    except OSError as error:
        if error.errno not in (errno.EPERM, errno.EACCES, errno.ENOTSUP, errno.ENOSYS) and getattr(error, "winerror", None) != 1314:
            raise
    else:
        config.available_features.add("symlinks")
config.substitutions.append((
    "%run_stage_spill",
    " ".join(shell_quote(value) for value in [config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_stage_spill.py"), config.pagosc, config.test_clang]),
))
config.substitutions.append((
    "%run_function_evidence",
    " ".join(shell_quote(value) for value in [config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_function_evidence.py"), config.pagosc]),
))
config.substitutions.append((
    "%run_snapshot",
    " ".join(shell_quote(value) for value in [config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_snapshot.py"), config.pagosc]),
))
config.substitutions.append((
    "%run_array_bounds",
    " ".join(shell_quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_array_bounds.py"),
        config.pagosc, config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_cache",
    " ".join(shell_quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_cache.py"),
        config.pagosc, config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_sections",
    " ".join(shell_quote(value) for value in [
        config.python, os.path.join(config.pagos_test_source_root, "Inputs", "run_sections.py"),
        config.pagosc, config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_globals",
    " ".join(shell_quote(value) for value in [
        config.python, os.path.join(config.pagos_test_source_root, "Inputs", "run_globals.py"),
        config.pagosc, config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_residual",
    " ".join(shell_quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_residual.py"),
        config.pagosc,
        config.test_clang,
    ]),
))
config.environment["PATH"] = os.pathsep.join(
    [config.llvm_tools_dir, config.environment.get("PATH", "")]
)
config.substitutions.append((
    "%check_object",
    " ".join(shell_quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "check_object.py"),
        config.pagosc,
        config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_native_opt",
    " ".join(shell_quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_native_opt.py"),
        config.pagosc,
        config.test_clang,
        config.test_lld,
        config.test_nm,
        config.test_objdump,
    ]),
))
config.substitutions.append((
    "%run_c_abi",
    " ".join(shell_quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_c_abi.py"),
        config.pagosc,
        config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_crc32",
    " ".join(shell_quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_crc32.py"),
        config.pagosc,
        config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_optimized",
    " ".join(shell_quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_residual.py"),
        "--optimize",
        config.pagosc,
        config.test_clang,
    ]),
))
