# Cortex-M33: emit and execute a Pagos object

Use this target for the integer/pointer C boundary on Cortex-M33:

```sh
build/debug/pagosc --target=thumbv8m.main-none-eabi --cpu=cortex-m33 \
  -O2 --relocation-model=static --code-model=small \
  emit-obj tests/lit/codegen/arm-c-abi.pgs -o build/arm.o
python3 scripts/check_arm.py --pagosc build/debug/pagosc --out build/arm-check
```

The second command runs QEMU's `mps2-an505` Cortex-M33 machine. Install
`qemu-system-arm`, the Arm bare-metal GCC toolchain and headers, Clang, LLD,
and LLVM inspection tools; the pinned CI image includes them. QEMU is pinned
to 11.1.1. The GCC multilib must provide `thumb/v8-m.main/nofp/libgcc.a`.
The [QEMU machine documentation](https://www.qemu.org/docs/master/system/arm/mps2.html)
describes the simulated board; it is not an MCXN947 peripheral model.

## Supported boundary: base AAPCS32, not arbitrary ARM

The validated configuration is little-endian Thumb ARMv8-M Mainline,
`cortex-m33`, bare-metal `eabi`, default CPU features (omit `--features`).
Object emission explicitly selects `aapcs` and the soft-float ABI. Supported
boundary types remain `u32/i32` and integer/record pointers, with exact public
names. There are no floating-point, aggregate-by-value or variadic boundary
signatures. The [base AAPCS32 specification](https://github.com/ARM-software/abi-aa/blob/2025Q1/aapcs32/aapcs32.rst)
defines the register/stack convention. Tests cross-link with Clang and GCC,
including ten arguments, signed values, pointer aliases and record layouts.

Hard-float EABI, other ARM CPUs/architectures and explicit feature overrides
remain outside this validated C ABI subset. Existing ARM/Thumb layout queries
remain available. `usize` is 32 bits; an integer-field record uses the target's
natural layout, including 8-byte alignment for `u64` storage. Ordinary unaligned
memory access keeps its existing semantics. Volatile access requires natural
alignment and a width no greater than the target pointer width: 8/16/32 bits
here, not 64. The compiler rejects over-wide volatile access before codegen.

Legacy emission plus all six explicit optimization levels execute in QEMU.
Use small/default code model, not medium (medium is x86-64/RISC-V only).
Stage decisions, accounting and C1–C4 protocols are unchanged; compiler-build
identity still rolls with compiler source changes.

## What the emulator checks

[check_arm.py](../scripts/check_arm.py) builds 63 Pagos objects and cross-links
292 firmware cases with Clang and GCC:

- C calls in both directions, recursive calls, pointers, aliases, arrays,
  integer records and 8/16/32-bit volatile accesses;
- CRC constants generated at Comptime and variable-length Runtime CRC inputs;
- real Pagos UART MMIO, bounded TX polling, initialized data and poisoned BSS;
- skipped data copy / BSS clear, deliberate failure and a marked infinite loop;
- null read/write, discarded null access, misaligned volatile access,
  out-of-range shift, division by zero and array bounds traps;
- ELF32 Arm/EABI flags, section kind, mutability, symbol size and alignment
  before and after linking.

The [startup](../platforms/qemu-cortex-m33/start.S) provides a secure reset vector,
8-byte-aligned stack, data copy and BSS clearing. The
[linker script](../platforms/qemu-cortex-m33/link.ld) uses AN505 memory:
code at `0x10000000` and data at `0x28000000`; UART0 is at `0x40200000`.
These addresses belong to this simulator, not FRDM-MCXN947 board support.

UART output is actual CMSDK MMIO. Semihosting supplies only process termination:
0=pass, 1=reported failure, 2=expected Pagos trap, 3=unexpected fault, 4=TX timeout.
Expected traps must be UsageFault with the LLVM `udf #254` instruction at the
saved PC; unrelated `udf #1` must instead report fault. The timeout case must
print its entry marker before the host deadline. A silent failed boot cannot
satisfy either test. The C glue supplies the `abort`/`memcpy` hooks referenced
by Arm libgcc's EHABI personality; this does not add a language exception model.

The startup and fault handlers are test-platform glue, not a supported interrupt,
RTOS, TrustZone transition, FPU, or multicore ABI. Emulator wall time is not
hardware frequency, cycle or power evidence.

## CI and board handoff

```sh
python3 scripts/ci.py --preset debug --quality --rv32 --arm
```

The Debug workflow includes ARM and the existing RV32 checks. Other compiler
configurations retain their existing checks; normal builds do not download
emulators or toolchains. On Linux, host test harnesses disable only their own
core dumps with `PR_SET_DUMPABLE`: kernel pipe-based crash collectors can block
expected trap processes even with `RLIMIT_CORE=0`. Signal assertions and short
execution deadlines remain active; compiler crash diagnostics are unaffected.

FRDM-MCXN947 work lives in the sibling `pagos-frdm-mcxn947-demo` repository.
That consumer owns NXP startup/boot metadata, MCXN947 memory and clocks,
LP-FLEXCOMM pins, LinkServer and physical-board evidence. AN505 firmware is
emulator-only. The first board slice uses CPU0 and polling UART; QEMU acceptance
allows board bring-up to proceed independently of the compiler ABI work.
