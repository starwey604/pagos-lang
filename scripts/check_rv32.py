#!/usr/bin/env python3
"""Run RV32 environment, Pagos/C ABI, and Pagos-native MMIO probes."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from elf_storage import verify_storage

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = ROOT / "platforms/qemu-rv32-virt"
ISA = ["-march=rv32imac", "-mabi=ilp32"]
CPU = "rv32i,m=true,a=true,c=true,zicsr=true,zifencei=true"


def checked(command):
    return subprocess.check_output(command, text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu-version", default="11.1.1")
    parser.add_argument("--pagosc", type=Path,
                        help="also execute Pagos/C ABI and native MMIO firmware")
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
        objects = {}
        harnesses = {}
        if args.pagosc:
            pagosc = str(args.pagosc.resolve())
            target = ["--target=riscv32-unknown-elf", "--features=+m,+a,+c"]
            for name, harness in (("c-abi", "c_abi.c"), ("pointers", "pointers.c"),
                                  ("global-arrays", "global_arrays.c"),
                                  ("mmio", None)):
                source = str(PLATFORM / "mmio.pgs" if harness is None else
                             ROOT / f"tests/lit/codegen/{name}.pgs")
                objects[name] = str(Path(directory) / f"{name}.o")
                objects[name + "-opt"] = str(Path(directory) / f"{name}-opt.o")
                if harness is not None:
                    harnesses[name] = harnesses[name + "-opt"] = str(ROOT / "tests/lit/Inputs" / harness)
                subprocess.run([pagosc, *target, "emit-obj", source, "-o",
                                objects[name]], check=True)
                ir = subprocess.check_output([pagosc, *target, "emit-llvm", source])
                subprocess.run(["clang", "--target=riscv32-unknown-elf", *ISA,
                                "-O2", "-c", "-x", "ir", "-", "-o",
                                objects[name + "-opt"]], input=ir, check=True)
                if name == "mmio":
                    placement = {
                        "boot_seed": (".data.boot", 64, True, False),
                        "boot_count": (".bss.boot", 128, True, True),
                        "poll_limit": (".rodata.uart", 32, False, False),
                        "boot_table": (".rodata.boot", 32, False, False),
                    }
                    for key in (name, name + "-opt"):
                        verify_storage(objects[key], placement)
                if name == "global-arrays":
                    for key in (name, name + "-opt"):
                        verify_storage(objects[key], {
                            "buffer": (".bss.buffer", 64, True, True),
                            "squares": (".rodata.squares", 32, False, False),
                            "words": (".data.words", 4, True, False),
                        })
        for compiler in ("clang", "riscv64-elf-gcc"):
            cases = [
                ("success", None, 0),
                ("failure", "FORCE_FAILURE", 1),
                ("bss-init", "SKIP_BSS_CLEAR", 1),
                ("data-init", "SKIP_DATA_COPY", 1),
                ("timeout", "FORCE_TIMEOUT", None),
            ]
            cases += [(name, "WITH_PAGOS_C_ABI", 0) for name in harnesses]
            if args.pagosc:
                cases += [(name, "WITH_PAGOS_MMIO", status)
                          for name in ("mmio", "mmio-opt") for status in (0, 1)]
            cases = [(case, define, expected, None) for case, define, expected in cases]
            if args.pagosc:
                cases += [(name, "WITH_PAGOS_MMIO", 1, fault)
                          for name in ("mmio", "mmio-opt")
                          for fault in ("SKIP_DATA_COPY", "SKIP_BSS_CLEAR")]
            for case, define, expected, fault in cases:
                elf = str(Path(directory) / f"{compiler}-{case}-{fault or expected}.elf")
                command = [compiler, *ISA, "-O2", "-ffreestanding", "-nostdlib",
                           "-fno-stack-protector", "-fno-pic", "-mcmodel=medany",
                           "-msmall-data-limit=0", "-Wl,--no-relax",
                           f"-Wl,-T,{PLATFORM / 'link.ld'}"]
                if compiler == "clang":
                    command += ["--target=riscv32-unknown-elf", "-fuse-ld=lld"]
                if define:
                    command += [f"-D{define}"]
                if define == "WITH_PAGOS_MMIO":
                    command += [objects[case]]
                    if fault:
                        command += [f"-D{fault}"]
                    elif expected == 1:
                        command += ["-DFORCE_FAILURE"]
                elif case in objects:
                    command += ["-DPAGOS_FREESTANDING", objects[case],
                                harnesses[case]]
                command += [str(PLATFORM / "start.S")]
                if define != "WITH_PAGOS_MMIO":
                    command += [str(PLATFORM / "smoke.c")]
                command += [str(runtime), "-o", elf]
                subprocess.run(command, check=True)
                if define == "WITH_PAGOS_MMIO":
                    verify_storage(elf, {
                        "boot_seed": (".data", 64, True, False),
                        "boot_count": (".bss", 128, True, True),
                        "poll_limit": (".rodata", 32, False, False),
                        "boot_table": (".rodata", 32, False, False),
                    })
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
                    if define == "WITH_PAGOS_MMIO":
                        output = output.replace("RV32", "MMIO")
                    if expected is None or result.returncode != expected or result.stdout != output:
                        raise RuntimeError(f"{compiler}/{case}: {result}")
                print(f"PASS {compiler}/{case}" +
                      (f"/{fault or f'exit-{expected}'}" if define == "WITH_PAGOS_MMIO" else ""), flush=True)


if __name__ == "__main__":
    main()
