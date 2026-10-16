import inspect
import os
import shlex

import lit.formats


config.name = "Pagos"
shell_options = {"execute_external": True}
if "force_execute_external" in inspect.signature(lit.formats.ShTest).parameters:
    shell_options["force_execute_external"] = True
config.test_format = lit.formats.ShTest(**shell_options)
config.suffixes = [".pgs"]
config.excludes = ["Inputs"]
config.test_source_root = config.pagos_test_source_root
config.test_exec_root = config.pagos_test_exec_root
config.substitutions.append(("%pagosc", config.pagosc))
config.substitutions.append(("%FileCheck", config.filecheck))
config.substitutions.append((
    "%run_stage_spill",
    " ".join(shlex.quote(value) for value in [config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_stage_spill.py"), config.pagosc, config.test_clang]),
))
config.substitutions.append((
    "%run_function_evidence",
    " ".join(shlex.quote(value) for value in [config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_function_evidence.py"), config.pagosc]),
))
config.substitutions.append((
    "%run_snapshot",
    " ".join(shlex.quote(value) for value in [config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_snapshot.py"), config.pagosc]),
))
config.substitutions.append((
    "%run_array_bounds",
    " ".join(shlex.quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_array_bounds.py"),
        config.pagosc, config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_cache",
    " ".join(shlex.quote(value) for value in [
        config.python,
        os.path.join(config.pagos_test_source_root, "Inputs", "run_cache.py"),
        config.pagosc, config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_sections",
    " ".join(shlex.quote(value) for value in [
        config.python, os.path.join(config.pagos_test_source_root, "Inputs", "run_sections.py"),
        config.pagosc, config.test_clang,
    ]),
))
config.substitutions.append((
    "%run_globals",
    " ".join(shlex.quote(value) for value in [
        config.python, os.path.join(config.pagos_test_source_root, "Inputs", "run_globals.py"),
        config.pagosc, config.test_clang,
    ]),
))
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
