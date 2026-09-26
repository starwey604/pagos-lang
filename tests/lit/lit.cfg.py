import os
import shlex

import lit.formats


config.name = "Pagos"
config.test_format = lit.formats.ShTest(execute_external=True)
config.suffixes = [".pgs"]
config.excludes = ["Inputs"]
config.test_source_root = config.pagos_test_source_root
config.test_exec_root = config.pagos_test_exec_root
config.substitutions.append(("%pagosc", config.pagosc))
config.substitutions.append(("%FileCheck", config.filecheck))
config.substitutions.append((
    "%run_residual",
    " ".join(shlex.quote(value) for value in [
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
    " ".join(shlex.quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "check_object.py"),
        config.pagosc,
        config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_c_abi",
    " ".join(shlex.quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_c_abi.py"),
        config.pagosc,
        config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_crc32",
    " ".join(shlex.quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_crc32.py"),
        config.pagosc,
        config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_optimized",
    " ".join(shlex.quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_residual.py"),
        "--optimize",
        config.pagosc,
        config.test_clang,
    ]),
))
