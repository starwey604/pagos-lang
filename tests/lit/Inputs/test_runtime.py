"""Capabilities shared by native-entry execution tests."""
import os
from pathlib import Path
import signal


def executable_path(path):
    return Path(str(path) + ".exe") if os.name == "nt" else Path(path)


def assert_trap(result):
    expected = 0xC000001D if os.name == "nt" else -signal.SIGILL
    actual = result.returncode & 0xFFFFFFFF if os.name == "nt" else result.returncode
    assert actual == expected, ("expected illegal-instruction trap", result)
