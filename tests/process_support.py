"""Host adapters for process failure and console interruption tests."""
from contextlib import contextmanager
import os
import signal
import subprocess


@contextmanager
def failing_stdout():
    if os.name != "nt":
        with open("/dev/full", "wb") as stream:
            yield stream
    else:
        reader, writer = os.pipe()
        os.close(reader)
        with os.fdopen(writer, "wb") as stream:
            yield stream


def interruptible_process(arguments, **kwargs):
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    return subprocess.Popen(arguments, creationflags=flags, **kwargs)


def interrupt_process(process):
    process.send_signal(signal.CTRL_BREAK_EVENT if os.name == "nt" else signal.SIGINT)


def require_posix_stop_continue():
    if not hasattr(signal, "SIGSTOP") or not hasattr(signal, "SIGCONT"):
        print("SKIP: POSIX stop/continue signals unavailable; ordinary query tests still run.")
        raise SystemExit(77)
