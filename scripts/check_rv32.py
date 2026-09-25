#!/usr/bin/env python3
"""Build and run the pinned, freestanding RV32 environment smoke tests."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = ROOT / "platforms/qemu-rv32-virt"
ISA = ["-march=rv32imac", "-mabi=ilp32"]
CPU = "rv32i,m=true,a=true,c=true,zicsr=true,zifencei=true"


def checked(command):
    return subprocess.check_output(command, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu-version", default="11.1.1")
    args = parser.parse_args()
    version = checked(["qemu-system-riscv32", "--version"]).splitlines()[0]
    if version != f"QEMU emulator version {args.qemu_version}":
        raise SystemExit(f"unexpected QEMU: {version}; review before overriding version")
    runtime = Path(checked(["riscv64-elf-gcc", *ISA, "-print-libgcc-file-name"]))
    if not runtime.is_file() or "rv32imac/ilp32" not in str(runtime):
        raise SystemExit(f"missing matching RV32 multilib: {runtime}")
    print(json.dumps({"qemu": version, "cpu": CPU, "isa": ISA,
                      "clang": checked(["clang", "--version"]).splitlines()[0],
                      "gcc": checked(["riscv64-elf-gcc", "--version"]).splitlines()[0],
                      "runtime": str(runtime)}, indent=2), flush=True)
    with tempfile.TemporaryDirectory(prefix="pagos-rv32-") as directory:
        for compiler in ("clang", "riscv64-elf-gcc"):
            for case, define, expected in (
                ("success", None, 0),
                ("failure", "FORCE_FAILURE", 1),
                ("bss-init", "SKIP_BSS_CLEAR", 1),
                ("data-init", "SKIP_DATA_COPY", 1),
                ("timeout", "FORCE_TIMEOUT", None),
            ):
                elf = str(Path(directory) / f"{compiler}-{case}.elf")
                command = [compiler, *ISA, "-O2", "-ffreestanding", "-nostdlib",
                           "-fno-stack-protector", "-fno-pic", "-mcmodel=medany",
                           "-msmall-data-limit=0", "-Wl,--no-relax",
                           f"-Wl,-T,{PLATFORM / 'link.ld'}"]
                if compiler == "clang":
                    command += ["--target=riscv32-unknown-elf", "-fuse-ld=lld"]
                if define:
                    command += [f"-D{define}"]
                command += [str(PLATFORM / "start.S"), str(PLATFORM / "smoke.c"),
                            str(runtime), "-o", elf]
                subprocess.run(command, check=True)
                qemu = ["qemu-system-riscv32", "-machine", "virt", "-cpu", CPU,
                        "-accel", "tcg", "-smp", "1", "-m", "16M", "-bios", "none",
                        "-kernel", elf, "-display", "none", "-monitor", "none",
                        "-serial", "stdio", "-nic", "none", "-no-reboot"]
                try:
                    result = subprocess.run(qemu, capture_output=True, text=True,
                                            timeout=1 if expected is None else 10)
                except subprocess.TimeoutExpired:
                    if expected is not None:
                        raise
                else:
                    output = "PAGOS RV32 PASS\n" if expected == 0 else "PAGOS RV32 FAIL\n"
                    if expected is None or result.returncode != expected or result.stdout != output:
                        raise RuntimeError(f"{compiler}/{case}: {result}")
                print(f"PASS {compiler}/{case}", flush=True)


if __name__ == "__main__":
    main()
