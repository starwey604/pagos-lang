import os

import lit.formats


config.name = "Pagos"
config.test_format = lit.formats.ShTest(execute_external=True)
config.suffixes = [".pgs"]
config.test_source_root = config.pagos_test_source_root
config.test_exec_root = config.pagos_test_exec_root
config.substitutions.append(("%pagosc", config.pagosc))
config.substitutions.append(("%FileCheck", config.filecheck))
config.environment["PATH"] = os.pathsep.join(
    [config.llvm_tools_dir, config.environment.get("PATH", "")]
)
