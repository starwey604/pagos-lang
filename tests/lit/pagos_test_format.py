import os
import hashlib
from pathlib import Path, PurePath
import shlex
import lit.formats
import lit.TestRunner


def shell_quote(value):
    # Only native Windows separators need conversion. A POSIX backslash is
    # part of the filename; preserve it when quoting for lit's internal shell.
    path = value if isinstance(value, PurePath) else Path(value)
    return shlex.quote(path.as_posix())


class PagosShTest(lit.formats.ShTest):
    def execute(self, test, lit_config):
        # Quote the default source/tmp paths once, including suffixes such as
        # %t.cache. RUN lines use these substitutions without extra quotes.
        # CAS/task namespaces append long digest paths. Keep per-case data
        # stems bounded instead of repeating the entire fixture filename.
        stem = hashlib.sha256(test.getFullName().encode("utf-8")).hexdigest()[:16]
        temporary = os.path.join(test.config.test_exec_root, "t")
        os.makedirs(temporary, exist_ok=True)
        tmp_base = os.path.join(temporary, stem)
        substitutions = [
            (r"%s(?![A-Za-z_])", shell_quote(test.getSourcePath())),
            (r"%S(?![A-Za-z_])", shell_quote(os.path.dirname(test.getSourcePath()))),
            (r"%t(?![A-Za-z_])", shell_quote(tmp_base + ".tmp")),
        ]
        return lit.TestRunner.executeShTest(
            test, lit_config, False, extra_substitutions=substitutions)
