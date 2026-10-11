#!/usr/bin/env python3
"""Exercise real cache files, process crashes, and competing publishers."""

from pathlib import Path
import subprocess
import sys
import tempfile
import time


def main():
    probe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="pagos-store-process-") as temporary:
        base = Path(temporary)

        def command(root, mode="write", inputs="module-v1", value="49",
                    action="", key="square"):
            return [probe, mode, str(root), inputs, value, action, key]

        def run(root, mode="write", inputs="module-v1", value="49",
                action="", key="square", expected=0):
            result = subprocess.run(command(root, mode, inputs, value, action, key),
                                    text=True, capture_output=True, timeout=15)
            assert result.returncode == expected, (result.args, result.returncode,
                                                   result.stdout, result.stderr)
            return result.stdout.strip()

        def await_file(path, process):
            deadline = time.monotonic() + 5
            while not path.exists():
                assert process.poll() is None, "publisher exited before ready"
                assert time.monotonic() < deadline, "publisher readiness timeout"
                time.sleep(0.005)

        root = base / "round-trip"
        assert run(root, "read", expected=1) == "not-found"
        assert not root.exists()
        assert run(root) == "published"
        assert run(root, "read") == "49"
        assert run(root) == "present"
        assert run(root, inputs="module-v2", value="64") == "published"
        assert run(root, "read", inputs="module-v2") == "64"
        assert run(root, value="50", expected=1) == "conflict"
        assert run(root, "read", expected=1) == "conflict"
        assert run(root, "read", inputs="module-v2") == "64"

        for action in ("crash-temporary", "crash-object", "crash-manifest-before",
                       "crash-manifest-after", "delete-tmp", "delete-index"):
            root = base / action
            run(root, action=action,
                expected=1 if action.startswith("delete") else 86)
            manifests = list(root.glob("queries/*/*/*.manifest"))
            if action == "crash-manifest-after":
                assert len(manifests) == 1
                assert run(root, "read") == "49"
            else:
                assert not manifests, "uncommitted publication became visible"
                assert run(root, "read", expected=1) == "not-found"
                assert run(root) == "published"
                assert run(root, "read") == "49"
            if action == "crash-temporary":
                assert list((root / "tmp").iterdir()), "crash fixture lost temporary"

        # Same-key writers serialize; different keys contend on shared CAS
        # objects using atomic no-replace publication instead of a global lock.
        for shared_key in (True, False):
            root = base / ("concurrent-key" if shared_key else "concurrent-cas")
            root.mkdir()
            processes = []
            try:
                for index in range(8):
                    key = "square" if shared_key else f"square{index}"
                    processes.append(subprocess.Popen(
                        command(root, action="gate", key=key),
                        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
                (root / "start").touch()
                for process in processes:
                    output, errors = process.communicate(timeout=15)
                    assert process.returncode == 0, (output, errors)
                    assert output.strip() in ("published", "present")
                assert len(list(root.glob("objects/*/*"))) == 2
                assert len(list(root.glob("queries/*/*/*.manifest"))) == (1 if shared_key else 8)
                assert not list((root / "tmp").iterdir())
                assert run(root, "read", key="square" if shared_key else "square0") == "49"
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.kill()
                    process.wait()

        root = base / "lock-owner"
        holder = subprocess.Popen(command(root, action="hold"),
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            await_file(root / "ready", holder)
            assert run(root, "read", expected=1) == "lock-busy"
            assert run(root, expected=1) == "lock-busy"
            holder.kill()
            holder.wait(timeout=5)
            assert run(root) == "published", "process death left a stale lock"
            assert run(root, "read") == "49"
        finally:
            if holder.poll() is None:
                holder.kill()
            holder.wait()

        # Competing different results must create a persistent conflict, even
        # when neither publisher saw an initial candidate before dispatch.
        root = base / "concurrent-conflict"
        root.mkdir()
        writers = [subprocess.Popen(command(root, value=value, action="gate"),
                   text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                   for value in ("49", "50")]
        try:
            (root / "start").touch()
            outputs = [writer.communicate(timeout=15)[0].strip() for writer in writers]
            assert sorted(outputs) == ["conflict", "published"], outputs
            assert sorted(writer.returncode for writer in writers) == [0, 1]
            assert run(root, "read", expected=1) == "conflict"
        finally:
            for writer in writers:
                if writer.poll() is None:
                    writer.kill()
                writer.wait()

        print("Cross-process recovery, publication, CAS races and conflicts passed.")


if __name__ == "__main__":
    main()
