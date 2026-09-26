# Development Guide

## Toolchain Contract

The Milestone 1 reference compiler is built as C++26. Supported development
configurations use Clang 22 or GCC 16 with LLVM 22.x. Code may use C++26
features implemented by both compilers; avoid relying on a single compiler's
experimental extension.

Required tools and libraries are:

- CMake 3.30 or newer and Ninja;
- LLVM 22 development headers, libraries, tools, and CMake configuration;
- GoogleTest for C++ unit tests;
- Python 3, LLVM `lit`, and `FileCheck` for language-level golden tests;
- Clang 22 and a host C linker/runtime for executing generated LLVM IR;
- `clang-format` and `clang-tidy` for local quality checks.

LLVM packages differ across distributions. CMake loads `LLVMConfig.cmake`,
checks `LLVM_VERSION_MAJOR`, and prefers the monolithic `LLVM` target when the
package enables `LLVM_LINK_LLVM_DYLIB`. Component libraries are the fallback.
Override discovery with `-DLLVM_DIR=/path/to/lib/cmake/llvm` when necessary.

## Dependency Policy

Use system packages for the compiler toolchain, LLVM, LLD, and QEMU. Do not
download or build these implicitly during normal CMake configuration. Keep
LLVM on the supported major version. Existing LLVM and GoogleTest discovery
continues to use `find_package`; no source fallback is currently required.

For future source dependencies, use CMake `FetchContent`, not Git submodules.
Centralize declarations in `cmake/Dependencies.cmake` when the first such
dependency is introduced. Pin Git dependencies to full commit hashes, or use
archives with `URL_HASH SHA256=...`; do not track moving branches. Review
license, provenance, transitive downloads, and upstream CMake code before
adoption. Commit dependency updates separately with regression evidence.

Provide explicit system-package and pinned-source modes rather than silently
choosing whichever version happens to be installed. CI must select and record
its mode. Keep downloads in the build tree and support pre-fetched checkouts
through `FETCHCONTENT_SOURCE_DIR_<NAME>`. An offline first configuration needs
the sources supplied beforehand; `FETCHCONTENT_FULLY_DISCONNECTED` does not
populate an empty cache. See the
[FetchContent reference](https://cmake.org/cmake/help/latest/module/FetchContent.html).

Keep project warnings target-scoped; do not impose Pagos `-Werror` on upstream
targets. Disable unnecessary dependency tests/examples. Vendor only small
sources that genuinely need repository-local maintenance, retaining licenses
and origin/version information. Build host dependencies and target runtimes
in separate configurations: an RV32 builtins archive is not a host library.

## Configure and Build

The default preset uses Clang and produces `build/debug/pagosc`:

```sh
cmake --preset debug
cmake --build --preset debug
```

Use `gcc-debug` for the second supported host compiler, `release` for optimized
builds, and `asan` for AddressSanitizer plus UndefinedBehaviorSanitizer.

## Run the Compiler

### RV32 environment probe

Run `python3 scripts/check_rv32.py` with QEMU 11.1.1, Clang/LLD 22, and
`riscv64-elf-gcc` providing the `rv32imac/ilp32` libgcc multilib. This builds
C/assembly with both Clang and GCC, then checks UART output, initialized data,
BSS clearing, stack use, 64-bit division support, failure exits, and timeouts.
The checked-in board support is under `platforms/qemu-rv32-virt/`; no libc,
OpenSBI, Linux, semihosting, or physical board is involved. Temporary outputs
are removed automatically. QEMU version changes require an explicit reviewed
`--qemu-version` override.

The configuration uses RV32IMAC/ILP32 (`riscv32-unknown-elf` with Clang), a
single TCG hart, 16 MiB RAM, `virt`, `-bios none`, and an ELF entry at
`0x80000000`. The CPU starts from `rv32i` with M/A/C/Zicsr/Zifencei explicitly
enabled. UART is at `0x10000000`; the separate test-finisher at `0x100000`
reports success/failure. These are virtual-board conventions, not language
semantics or evidence of CH32 support. See the
[QEMU board source](https://gitlab.com/qemu-project/qemu/-/blob/v11.1.0/hw/riscv/virt.c)
and [test device](https://gitlab.com/qemu-project/qemu/-/blob/v11.1.0/hw/misc/sifive_test.c).

This probe checks the environment only; Pagos firmware generation belongs to
M3. The matching target libgcc is currently linked explicitly; the installed
host compiler-rt must not be substituted for it.

### Host compiler commands

All commands accept explicit `--target=TRIPLE`, `--cpu=CPU`, and
`--features=+feature,-feature` options. Omitting the triple uses the host
triple with a generic CPU; no host `sizeof` values define target layout.
For example:

```sh
build/debug/pagosc emit-llvm benchmarks/cache-hits.pgs \
  --target=riscv32-unknown-elf --cpu=generic-rv32 --features=+m,+a,+c
build/debug/pagosc emit-hir tests/lit/stage/usize-static.pgs \
  --target=riscv64-unknown-elf
```

Preparation supports X86, RISC-V, and little-endian ARM/Thumb layout queries.
Unknown triples, CPUs, and feature names fail explicitly before type checking.
`usize` literal ranges, Static arithmetic, and aggregate budgets follow the
selected pointer width. Use the same target options for checking and emission;
omitting them selects the host target, not a platform-independent default.
LLVM IR includes the matching DataLayout, triple, and CPU
attributes. This is not yet object emission or full target C ABI support.

To inspect real internal function calls, emit MIR or LLVM IR for
`tests/lit/codegen/residual-call-scalar.pgs`. Runtime scalar calls now produce
`pagos.<name>.<id>` helpers; Static arguments are embedded in their bodies.
Names are module-local implementation details, not linkable C API names.

LLVM builds must include X86, RISCV, and ARM backends. Configure with
`-DPAGOS_USE_LLVM_DYLIB=OFF` to exercise component-library linking instead of
the preferred monolithic shared library.
This requires a complete component SDK. The current Arch packages omit Core
and target archives; on these packages the explicit component mode fails at
configuration with a missing-component diagnostic. Its full link has not
been validated; the monolithic path is the supported, tested baseline.

```sh
build/debug/pagosc check source.pgs
build/debug/pagosc emit-hir source.pgs
build/debug/pagosc explain-stage source.pgs
build/debug/pagosc emit-mir source.pgs
build/debug/pagosc emit-llvm source.pgs
```

Bound compile-time work with options placed anywhere before or after the
command:

```sh
build/debug/pagosc --max-fuel=100000 check source.pgs
build/debug/pagosc --max-recursion-depth=64 explain-stage source.pgs
build/debug/pagosc --max-specializations=512 emit-mir source.pgs
build/debug/pagosc --max-residual-specializations=512 emit-mir source.pgs
build/debug/pagosc --max-array-elements=4096 --max-array-bytes=16384 check source.pgs
build/debug/pagosc --max-aggregate-members=8192 --max-aggregate-bytes=32768 check source.pgs
```

`explain-stage` ends with deterministic analysis statistics for fuel,
specializations, cache hits, maximum recursion depth, reserved array elements,
and reserved array data bytes. Array limits are cumulative construction
quotas, not process-memory caps. Defaults are 65,536 elements and 262,144 bytes;
integers cost width/8 bytes and `bool` one logical byte per planned element.
Reservations precede expansion and are not refunded on early return. Cache
hits and aliases do not reconstruct arrays. A zero array quota permits scalar
code, but rejects any analyzed array construction.

The residual-definition limit defaults to 4,096, independently of the Static
specialization limit. `residual-specializations` counts successfully analyzed
Runtime scalar versions; `residual-cache-hits` counts their reuse, including
active recursive backedges. A hit keeps
argument work but skips body analysis and another version charge. A miss is
charged after successful analysis; recursive chains reserve slots before
following recursive edges. Active reservations share the limit with completed
versions. Fuel/depth/construction costs still apply. Exceeding the limit reports
`E4012`. Zero allows pure Static and non-recursive inline
Static-result calls. This quota is not a generated-byte or total-memory limit.
All-Static-argument misses still consume the existing Static specialization
budget even when the analyzed body produces a Runtime result.
Runtime recursion does not repeatedly enter analysis for an active key, so
`--max-recursion-depth` limits compiler analysis depth, not target stack depth.
No runtime stack guard or termination guarantee is provided.

Shared array/record quotas additionally default to 65,536 aggregate members and
262,144 logical bytes. `explain-stage` appends `aggregate-constructions`,
`aggregate-members`, and `aggregate-bytes`; array counters are their array-only
subset. `E4010` reports a shared quota failure. Both quota sets must permit an
array: raising a legacy array limit does not raise a shared limit. Records are
unaffected by array-only quotas. See [resource budgets](resource-budgets.md) for
examples, failure precedence, and accounting boundaries.

`external_input()` is the temporary Milestone 1 runtime-source intrinsic. It
returns `u32` and lowers to a declaration of `pagos_external_input` in LLVM IR.

For the fixed-length array slice, inspect the folded and residual examples:

```sh
build/debug/pagosc emit-hir tests/lit/stage/static-array.pgs
build/debug/pagosc emit-mir tests/lit/stage/array-lookup.pgs
build/debug/pagosc emit-llvm tests/lit/stage/array-lookup.pgs
```

The lookup example embeds one read-only table and retains a checked Runtime
index. Arrays support nonempty one-dimensional integer/`bool` values;
Static-index reads preserve individual element stages through aliases and
direct calls. Inspect `tests/lit/stage/array-projection.pgs` for a Static
result that still retains an unrelated Runtime input read. Nested arrays and
element mutation remain deferred.

Generate a table with `[for i in 0..256 { i * i }]`, or use Static expressions
such as `0..1 << bits`. Bounds evaluate once before reservation; Runtime bounds
report `E2002` with a dependency path. Source annotations still use literal
lengths, checked against the resolved shape. Each iteration consumes fuel as
well as the normal body cost; literals, generators, and Runtime array
constructions share the array quotas. Inspect both table examples:

```sh
build/debug/pagosc explain-stage tests/lit/stage/generated-table.pgs
build/debug/pagosc emit-llvm tests/lit/stage/generated-table.pgs
build/debug/pagosc emit-hir tests/lit/stage/generator-computed.pgs
build/debug/pagosc emit-llvm tests/lit/stage/generator-computed.pgs
```

`E4008` means the next array reservation would exceed a quota; its label shows
the request, already-reserved amounts, and both configured limits. Increase
limits deliberately. AST, HIR, cache copies, and LLVM memory are not counted.

## Test and Check

### Compiler performance baseline

Build `release`, then run:

```sh
python3 scripts/benchmark.py --output build/benchmarks/baseline.json
```

The Linux measurement script uses Python and GNU `/usr/bin/time`. It measures
five bounded workloads across `check`, `emit-mir`, and `emit-llvm`, with two
warmups and seven samples each. JSON records median/range, per-process peak
RSS, output sizes, stage counters, input/binary hashes, budgets, machine, and
tool versions. Wall time includes process startup and the measurement wrapper;
differences between commands are not precise phase timings. Avoid concurrent
builds/tests and compare repeated runs on the same machine. Reports stay under
ignored `build/`; keep only concise conclusions in the preparation plan.
Use `--reference-compiler /path/to/old/build/release/pagosc` for alternating
old/new samples with reversed order on each repeat. Both binaries must be
Release builds; results include the reference hash/revision and paired ratios.
This helps distinguish machine-wide fluctuations from implementation changes.

### Regression commands

`python3 scripts/ci.py --preset debug --quality --rv32` runs a clean build,
the complete test suite, benchmark-source checks, quality checks, and the
RV32 probe. Repeat with `--preset gcc-debug`, `asan`, and `release` for the
full matrix. Logs and tool versions remain under `build/ci-logs/`; temporary
build trees are removed on success or failure. Leak checks remain enabled.

The compiler workflow runs debug/quality/RV32 on PRs and the full matrix on
push, weekly schedule, or manual dispatch. Its Dockerfile fixes an amd64 base
image digest and the Arch archive dated 2026-09-25, independent of runner
toolchains. Refresh both deliberately; pinned images still need security
updates. Build the image with network access, then run tests without network:

```sh
docker build -f ci/Dockerfile -t pagos-ci .
docker run --rm --network none --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" pagos-ci python3 scripts/ci.py --preset debug --quality --rv32
```

Local script/container success is not a claim that GitHub Actions has run.
Workflow execution on GitHub requires pushing the commits separately.

Inspect the [minimal record example](records.md) with:

```sh
build/debug/pagosc explain-stage tests/lit/stage/record-config.pgs
build/debug/pagosc emit-mir tests/lit/stage/record-config.pgs
build/debug/pagosc emit-llvm tests/lit/stage/record-config.pgs
```

The [CRC-32 walkthrough](crc32.md) demonstrates a complete generated table,
Static check vector, Runtime checksum, and budget failures. Its differential
test requires only the existing Python standard library (`zlib`) and host Clang;
no additional package is needed. It checks external `-O0` and `-O2` execution.

```sh
ctest --preset debug
ctest --preset gcc-debug
python3 scripts/check_docs.py
clang-format --dry-run --Werror $(rg --files -g '*.h' -g '*.cpp')
clang-tidy --quiet -p build/debug $(rg --files lib tools -g '*.cpp')
```

GoogleTest covers C++ units such as lexing, source locations, evaluator
arithmetic, and MIR verification. `lit` and `FileCheck` cover complete `.pgs`
programs, stable diagnostics, typed HIR, residual SSA MIR, and LLVM IR. When a
restricted environment cannot run LeakSanitizer, pass
`ASAN_OPTIONS=detect_leaks=0` only for that run; do not disable leak checks in
normal CI.

The lit substitution `%run_residual %s <result|trap> [input ...]` compiles
emitted LLVM IR with a small host harness. It checks the result or trap and
the exact input-read sequence, rejecting missing or extra reads. Execution
has a timeout to catch broken loop backedges. The harness lives in
`tests/lit/Inputs/`; it exercises alias identity, evaluation order, short
circuiting, loop boundaries, discarded results, and specialization-cache hits.
