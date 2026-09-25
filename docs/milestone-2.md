# Milestone 2 Baseline

Status: core baseline complete, 2026-09-26. This is a research compiler baseline,
not a stable language release. Total compiler-memory limits are explicitly
deferred, not fulfilled by the construction quotas below.

## Implemented

- Explainable Static/Runtime analysis, stage-polymorphic calls, bounded Static
  recursion, and pure Static-result caching that preserves argument effects.
- Static and Runtime loops, ordered residual effects, and per-call early returns.
- Fixed-length scalar arrays and immutable named scalar-field records, with
  element/field-sensitive staging and cross-stage constant embedding.
- Array generators with computed Static bounds, per-specialization lengths,
  shape validation, and checked Runtime indexing.
- Verified SSA MIR, host LLVM IR, and `explain-stage`, `emit-hir`, `emit-mir`,
  and `emit-llvm` inspection commands.
- Fuel, recursion-depth, specialization, and cumulative aggregate construction
  quotas. These limit analysis work and logical payload, not process memory.

The [CRC-32 case](crc32.md) generates its 256-entry table at compile time and
checks Runtime messages against zlib. The [record case](records.md) demonstrates
mixed Static/Runtime configuration without losing initializer effects.

## Not implemented

- Total compiler-memory accounting or a process-memory cap: constant/cache
  copies, IR/container overhead, and LLVM allocations remain unbounded by a
  memory quota. Revisit with larger system-build workloads; see
  [resource scope](resource-budgets.md#remaining-memory-work).
- Nested records, aggregate-valued fields, arrays of records, mutation, dynamic
  arrays, dependent array signatures, and Runtime recursion.
- Compile-time filesystem/environment capabilities and dependency-aware caching.
- Stable record layout, C ABI, MMIO, cross-target object/link output, or firmware
  validation. These belong to subsequent systems work.

## Validation

Baseline regression: 50 GoogleTest units and 202 lit tests on Clang debug,
GCC debug, and ASan/UBSan. Residual execution tests include external Clang
`-O0`/`-O2`, exact input ordering, traps, and CRC differential checks.
Changed C++ passes clang-format and clang-tidy; documentation and whitespace
checks pass. Reproduce using the commands in [development](development.md).
