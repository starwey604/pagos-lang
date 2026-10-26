# Development Guide

## Toolchain Contract

The Milestone 1 reference compiler is built as C++26. The pinned CI baseline
uses Clang 22 or GCC 16 with LLVM 22.1.8. Local development also supports
Clang 23 with LLVM 23.x. Code may use C++26 features implemented by both
compiler families; avoid relying on a single compiler's
experimental extension.

Required tools and libraries are:

- CMake 3.30 or newer and Ninja;
- LLVM 22 or 23 development headers, libraries, tools, and CMake configuration;
- GoogleTest for C++ unit tests;
- Python 3, LLVM `lit`, and `FileCheck` for language-level golden tests;
- Clang 22 or 23 and a host C linker/runtime for executing generated LLVM IR;
- `clang-format` and `clang-tidy` for local quality checks.

LLVM packages differ across distributions. CMake loads `LLVMConfig.cmake`,
accepts major versions 22 and 23, and prefers the monolithic `LLVM` target when
the package enables `LLVM_LINK_LLVM_DYLIB`. Component libraries are the fallback.
Override discovery with `-DLLVM_DIR=/path/to/lib/cmake/llvm` when necessary.

## Dependency Policy

Use system packages for the compiler toolchain, LLVM, LLD, and QEMU. Do not
download or build these implicitly during normal CMake configuration. Keep
LLVM on a supported major version. Existing LLVM and GoogleTest discovery
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

If multiple LLVM installations are present, select the system SDK explicitly:

```sh
cmake --preset debug -DLLVM_DIR="$(llvm-config --cmakedir)"
```

This also works with LLVM 23 on rolling distributions without downgrading
system packages. The selected SDK must include the X86, RISCV, and ARM backends;
a GPU-only SDK such as ROCm's LLVM may not satisfy that requirement. CMake
retains the selected path in the build cache.

Use `gcc-debug` for the second supported host compiler, `release` for optimized
builds, and `asan` for AddressSanitizer plus UndefinedBehaviorSanitizer.

## Run the Compiler

### Experimental Stage cache

The cache is off by default. C2a admits internal pure leaf functions with all
Comptime scalar arguments, a scalar result, and a statement-free arithmetic,
comparison, conversion, or parameter expression body. Unsupported programs use
normal analysis. See the [C2a implementation](cache-scalar-c2a.md) for the exact
admission rules and resource protocol. [C2b](cache-array-c2b.md) extends this
to a single fixed-length scalar array literal or arithmetic generator. Ordinary
calls, local statements, array parameters, and array-call arguments carrying
residual work remain outside admission.

```sh
build/debug/pagosc --stage-cache=read-write --cache-dir=.pagos-cache \
    --cache-stats explain-stage tests/lit/cache/pure-scalar.pgs
build/debug/pagosc --stage-cache=read-only --cache-dir=.pagos-cache \
    --cache-stats explain-stage tests/lit/cache/pure-scalar.pgs
```

`--stage-cache=off|read-write|read-only` selects the experimental mode; enabling
it requires an explicit non-empty `--cache-dir=PATH`. The root `.pagos-cache/`
is ignored by Git. Off mode performs no cache IO, and read-only mode does not
create directories or publish objects. Storage failures fall back to analysis.
The existing semantic budgets apply on cold and hot runs, including fuel,
recursion depth, specialization count, and array/shared aggregate quotas.

`--cache-stats` writes one separate `stage-cache:` line to stderr after Stage
analysis. `hits` and `bodies-skipped` count successful disk reuse;
`body-evaluations` counts actual function-body analyses, including unsupported
functions. The explain-stage AnalysisStats line keeps its original logical
meaning and session-local hit counts. Without the telemetry flag, cache outcomes
do not add output or diagnostics. C2b adds opt-in physical phase timings; see its
implementation record for measured results and their limits.

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

Without extra arguments this remains a ten-case environment probe. Add
`--pagosc build/debug/pagosc` to run twelve additional Pagos/C ABI firmware cases:
direct object output and Clang `-O2` optimized IR, each linked against Clang-
and GCC-compiled C. They exercise exports/imports, recursion, signed values,
ten-argument register/stack passing, and effect order. The pointer suite also
executes Pagos RAM loads/stores, aliasing, recursive/loop updates, integer-address
round trips, and unaligned access. These ABI cases use C/assembly device support.
The global-array suite shares Pagos-owned buffers and generated tables with C,
checking element strides, alias snapshots, repeated effects and target `usize`.
Eight more cases run `platforms/qemu-rv32-virt/mmio.pgs` directly: Pagos polls
UART, prints `PAGOS MMIO PASS` or `PAGOS MMIO FAIL`, then writes the test device.
Both exit paths run with direct objects and optimized IR under both toolchains,
without linking the C smoke harness. Startup/linker support remains shared.
Eight further startup-fault cases omit data copying or BSS clearing for the
Pagos-owned globals, under both emission paths and both toolchains. Volatile
boot-state reads prevent constant folding from hiding incorrect initialization.
The globals use named data subsections with explicit alignment. Shared
`scripts/elf_storage.py` checks object and linked-image section membership,
flags, storage kind and symbol alignment before execution; no new tools are
required beyond LLVM `llvm-readobj`.
Boot data/BSS now contain arrays; volatile checks inspect both first and last
elements, plus a generated readonly table. Global record tests also compare C
field offsets and padded object sizes, and repeat data/BSS fault injection with
volatile record-field reads. Record-pointer tests exchange C-owned and Pagos-owned
state through typed pointers, including recursive helpers and pointer returns.
The total with `--pagosc` is 54 cases.
`scripts/ci.py --rv32` includes all of them.
The matching target libgcc is linked explicitly; do not substitute host compiler-rt.

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
`usize` literal ranges, Comptime arithmetic, and aggregate budgets follow the
selected pointer width. Use the same target options for checking and emission;
omitting them selects the host target, not a platform-independent default.
LLVM IR includes the matching DataLayout, triple, and CPU
attributes. `emit-obj` now emits a matching object directly through LLVM;
full target C ABI classification is still outside the supported subset.

To inspect real internal function calls, emit MIR or LLVM IR for
`tests/lit/codegen/residual-call-scalar.pgs`. Runtime scalar calls now produce
`pagos.<name>.<id>` helpers; Comptime arguments are embedded in their bodies.
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
build/debug/pagosc explain-provenance source.pgs
build/debug/pagosc emit-mir source.pgs
build/debug/pagosc emit-llvm source.pgs
build/debug/pagosc emit-obj source.pgs -o source.o
```

`emit-obj` requires exactly one `-o` path and never writes binary stdout.
Compilation failures preserve existing output; a successful write atomically
replaces it via an exclusive `<output>.tmp` sibling. An existing temporary,
missing parent directory, or input/output alias is an error. Text commands do
not accept `-o`. Objects use PIC relocation; this command adds no `-O2` IR pass
pipeline. Link with a matching external toolchain and required target runtime.

For the [minimal C ABI example](../tests/lit/codegen/c-abi.pgs):

```sh
build/debug/pagosc emit-obj tests/lit/codegen/c-abi.pgs -o build/c-abi.o
clang build/c-abi.o tests/lit/Inputs/c_abi.c -o build/c-abi
build/c-abi
python3 scripts/check_rv32.py --pagosc build/debug/pagosc
```

`extern fn` imports and `export fn` exports use exact source symbol names,
accept `u32/i32` and integer/record pointers, and always denote Runtime calls.
Validated targets are x86-64 Linux LP64 and generic RV32 ELF ILP32 with I/M/A/C.
The C prototype and
target flags must match; neither headers nor cross-language signature checks
are generated. Boundary-only modules omit `pagos_main`, allowing separate
objects to link. See [boundary rules](grammar.md#minimal-c-abi-milestone-3).

For memory access, substitute `tests/lit/codegen/pointers.pgs` and
`tests/lit/Inputs/pointers.c` in the native link example above. The C harness
owns the storage; Pagos uses `*const T` / `*mut T`, `*p` and `*p = value;`.
No `unsafe` keyword or borrowing annotations are required. Read the
[memory rules](grammar.md#raw-pointers-and-memory-milestone-3) before passing
addresses: null checks are not lifetime/bounds checks, and ordinary memory
operations are neither volatile nor atomic.

For the standalone Pagos device example, run
`python3 scripts/check_rv32.py --pagosc build/debug/pagosc`. No physical board
or new dependency is needed. Volatile tests also inspect optimized LLVM IR and
RV32 instructions using the existing LLVM `opt`/`llc` tools. Consult the
[volatile contract](grammar.md#volatile-access-and-mmio-milestone-3): accesses
must have device-valid widths and natural alignment, and provide no CPU fence,
cache setup or atomic synchronization.

`tests/lit/codegen/globals.pgs` and `tests/lit/Inputs/globals.c` exercise storage
owned by Pagos rather than C. The companion test runner inspects ELF sections,
symbols and relocations with LLVM `llvm-readobj`, then links two independently
compiled Pagos modules to check private-object identity and C pointer access.
No new dependency is required. Initializers must be pure Comptime; global reads
and `&name` are Runtime. See the
[global storage contract](grammar.md#global-integer-storage-milestone-3).

`tests/lit/codegen/global-sections.pgs` additionally checks named ELF data
subsections and explicit alignment on x86-64 and RV32, using direct objects and
optimized IR. Its native C harness checks initial contents, distinct readonly
objects, actual pointer alignment and persistent writes. RV64 global-only object
emission is tested separately; this does not expand the supported C ABI.
`align` is a minimum request, and `section` does not retain unused objects.
See [placement rules](grammar.md#data-subsections-and-alignment).

`tests/lit/codegen/global-arrays.pgs` and its C harness cover array element
addresses and mutation through the existing scalar-pointer ABI. Native negative
runs verify read/store/address/discard bounds traps before and after optimization.
Separate fixtures check index evaluation order and suppression of store-value
evaluation after an out-of-range address. See
[global array rules](grammar.md#global-integer-arrays); this is not array-by-value
C ABI support or local array address-taking.

`tests/lit/codegen/global-records.pgs` and its C harness exercise mixed-width
integer record storage, field pointers, snapshots, foreign mutation, early
returns and local shadowing. ELF symbol sizes and `TargetLayout` unit tests
include tail padding; C `offsetof` checks actual field addresses on the host and
RV32, both before and after optimization. Existing `llvm-readobj`, Clang/GCC and
QEMU suffice; no new dependency is needed. See
[global record rules](grammar.md#global-integer-field-records). This does not
enable record-by-value C calls.

`tests/lit/codegen/record-pointers.pgs` and its C harness extend that coverage to
whole-record addresses and field access through function parameters/results.
Host tests also check null read/address/discard/store traps, address-before-RHS
ordering, early returns, ordinary unaligned access and volatile alignment traps,
both before and after optimization. C never dereferences an unaligned struct
pointer: the unaligned test passes byte storage and converts inside Pagos.
No new dependency is required. See
[record pointer rules](grammar.md#record-pointers-and-field-access).

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

The residual-definition limit defaults to 4,096, independently of the Comptime
specialization limit. `residual-specializations` counts successfully analyzed
Runtime scalar versions; `residual-cache-hits` counts their reuse, including
active recursive backedges. A hit keeps
argument work but skips body analysis and another version charge. A miss is
charged after successful analysis; recursive chains reserve slots before
following recursive edges. Active reservations share the limit with completed
versions. Fuel/depth/construction costs still apply. Exceeding the limit reports
`E4012`. Zero allows pure Comptime and non-recursive inline
Comptime-result calls. This quota is not a generated-byte or total-memory limit.
All-Comptime-argument misses still consume the existing Comptime specialization
budget even when the analyzed body produces a Runtime result.
Runtime recursion does not repeatedly enter analysis for an active key, so
`--max-recursion-depth` limits compiler analysis depth, not target stack depth.
No runtime stack guard or termination guarantee is provided.
Each exported root reserves one slot and counts on successful body analysis;
extern declarations and calls to fixed boundary symbols consume no new slots.

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
Comptime-index reads preserve individual element stages through aliases and
direct calls. Inspect `tests/lit/stage/array-projection.pgs` for a Comptime
result that still retains an unrelated Runtime input read. Nested arrays and
element mutation remain deferred.

Generate a table with `[for i in 0..256 { i * i }]`, or use Comptime expressions
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

The Linux measurement script uses Python and per-child `os.wait4`. It measures
seven bounded workloads across `check`, `emit-mir`, and `emit-llvm`, with two
warmups and seven samples each. JSON records all samples plus median/range,
per-process peak RSS, output sizes, stage counters, input/binary hashes, budgets,
machine, and
tool versions. Wall time includes process startup, output capture and wait4
polling at a 1 ms interval. Differences between commands are not precise phase
timings. Avoid concurrent
builds/tests and compare repeated runs on the same machine. Reports stay under
ignored `build/`; keep only concise conclusions in the preparation plan.
Use `--reference-compiler /path/to/old/build/release/pagosc` for alternating
old/new samples with reversed order on each repeat. Both binaries must be
Release builds; results include the reference hash/revision and paired ratios.
This helps distinguish machine-wide fluctuations from implementation changes.

Use `--cache-modes=all` for independent off/cold/hot measurements. Cold clears
its disk cache before each child; hot uses a separately seeded read-only cache.
OS page caches are uncontrolled. Samples include disk file counts/bytes and
`--cache-stats` phase times; read/publish include codecs and checksums, while
nested body evaluation times overlap. `--case benchmarks/closed-generator.pgs`
and `--command check` select workloads/commands; both options are repeatable.
See [C2b measurements](cache-array-c2b.md) for phase boundaries and results.

The [C3q query lab](provenance-query-c3q.md) separates formula preparation from
resumable and compressed query costs, including selected invocation paths:

```sh
cmake --build --preset release
python3 scripts/benchmark_provenance_queries.py \
  --output build/benchmarks/provenance-c3q.json
```

It requires Linux `wait4` for per-child peak RSS. Experimental query flags apply
only to `explain-provenance`; default compiler behavior remains unchanged.

C3a evidence snapshots and C3b closed scalar residual templates are documented
in [snapshot implementation](provenance-snapshot-c3a.md) and
[residual implementation](cache-residual-c3b.md). Snapshot reading is an explicit
experiment which parses/checks current identity and restores evidence without
Stage analysis. Residual reuse remains controlled by stage-cache, default off.

```sh
build/debug/pagosc --snapshot-write=build/evidence explain-provenance example.pgs
build/debug/pagosc --snapshot-read=build/evidence explain-provenance example.pgs
cmake --build --preset release
python3 scripts/benchmark_evidence.py --output build/benchmarks/evidence-c3.json
```

The measurement separates snapshot encoding/checksum/restoration/query, whole
filesystem restoration, and off/cold/hot residual reuse. Snapshot IO currently
reads full objects; borrowed section spans do not imply lazy disk access.

[C4a3 function evidence](provenance-function-queries-c4a3.md) adds a separate
function/SCC format and a lazy reader. Reading parses/typechecks the current
source identity, loads required hot summaries and skips Stage. Cold topology is
read only for the selected query; decisions use hot metadata alone.

```sh
build/debug/pagosc --evidence-write=build/function-evidence \
  --evidence-root=aa explain-provenance example.pgs
build/debug/pagosc --evidence-read=build/function-evidence \
  --evidence-root=aa --evidence-mode=witness --max-fuel=0 \
  --provenance-slice=128 --provenance-query-stats \
  explain-provenance example.pgs
cmake --build --preset release
python3 scripts/benchmark_function_evidence.py \
  --output build/benchmarks/function-evidence-c4a3.json
```

`--evidence-mode` accepts `sources`, `witness`, `topology` and `decisions`.
For decisions, `--evidence-root` selects a scope name, including `module`;
all same-name specializations are listed. `--evidence-cache-bytes=N` bounds
retained decoded SCC buffers (default 4 MiB, zero allowed), not whole-process
memory. Snapshot and function evidence flags are mutually exclusive. The
benchmark compares lazy queries with full graph restoration and reports
per-child RSS including startup and source checking; no construction spill is
implemented.

[C4b1 construction storage](provenance-spill-c4b1.md) adds the C++
`EvidenceBuilder` API for append-time sealing, temporary spill, pinned old
blocks and delayed summary binding. That iteration supplied storage only. [C4b2](provenance-recorder-spill-c4b2.md) now
integrates Stage Recorder and explicit CLI controls:

```sh
build/debug/pagosc --evidence-spill=build/evidence-tmp \
  --spill-block-bytes=65536 --spill-resident-bytes=8388608 \
  --spill-cache-bytes=1048576 --spill-disk-bytes=268435456 \
  --spill-on-error=memory --spill-stats check example.pgs
```

The resident buffer limit excludes hot metadata, external pins and codec
temporaries. Native source/topology queries retain the disk view; explicit
compressed queries and snapshot/function exports restore the compatibility
graph. Stage continues when evidence becomes unavailable, while related
semantic publications are withheld. C4b3a adds shared RuntimeTrace prefixes and
real Stage measurements. C4b3b adds retained-work accounting, crash/cleanup
validation and measured large-graph peak savings; memory remains the default.

### Regression commands

The independent provenance lab compares trace paths, an append-only ID graph,
and selective evidence interning. See the
[prototype experiment](provenance-prototype.md) for build commands, query
coverage, reproducible measurements, and current limitations.

The compiler also records a frozen evidence snapshot during Stage analysis.
`explain-provenance` queries all recorded value sources and residual work for
top-level bindings, local Comptime contracts, and storage initializers. It can
show partial analysis alongside staging errors. See
[Provenance analysis](provenance-analysis.md) for ownership, call contexts,
byte-offset locations, query budgets, and recursive coverage limits.

`python3 scripts/ci.py --preset debug --quality --rv32` runs a clean build,
the complete test suite, benchmark-source checks, quality checks, and the
RV32 probe. Repeat with `--preset gcc-debug`, `asan`, and `release` for the
full matrix. Logs and tool versions remain under `build/ci-logs/`; temporary
build trees are removed on success or failure. Leak checks remain enabled.

The CI script deliberately requires the pinned LLVM 22.1.8 baseline. For local
LLVM 23 development, use the direct CMake, CTest, and quality commands instead.

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
Comptime check vector, Runtime checksum, and budget failures. Its differential
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

## Stage path and spill measurements

[C4b3a](provenance-trace-c4b3a.md) measures real Stage construction independently
from native evidence queries and an optional retained-path census:

```sh
cmake --build --preset release --target pagos_stage_spill_bench
python3 scripts/benchmark_stage_spill.py \
  --output build/benchmarks/stage-spill.json
```

The script records every sample and build/source identity. Build/query children
never allocate the census traversal index. Whole-process RSS includes the front
end and HIR; build-plus-query RSS is not an isolated query peak. An optional
`--baseline` and matching `--baseline-identity` compare a preserved earlier
Release probe. The archived report contains that dirty baseline's hashes; HEAD
alone cannot reconstruct it. CAS counters are logical payload IO, not total
system IO. C4b3b supplies the broader workload and IO report below.

[C4b3b](provenance-working-set-c4b3b.md) adds the broader real Stage corpus with
Recorder definition identity, separate build/query RSS windows, capacity census,
and Linux process IO counters. Temporary files default to `build/benchmarks/`;
use `--work-dir` to select the filesystem being measured.

```sh
python3 scripts/benchmark_stage_working_set.py \
  --output build/benchmarks/stage-working-set.json
build/debug/pagosc reclaim-evidence build/evidence-tmp
```

Reclamation is explicit and only removes recognized, unlocked task namespaces;
active views/builders and unknown markers are skipped. A killed process leaves
its directory until explicit maintenance. Unmarked early-creation debris and
legacy directories require offline inspection. The CAS disk quota excludes the
26-byte task marker and filesystem metadata. `--spill-stats` adds an opt-in
metadata/working-table capacity census; these capacities are not a total RSS
limit. Large-graph peak savings meet C4b's experimental gate; no general speedup
or small-task memory benefit is claimed.

[C4c1](provenance-checkpoint-c4c1.md) exposes library-only query checkpoints on
validated function evidence. Create a binding from the complete current source,
construct the opt-in `QueryTask`, and explicitly encode/publish its dedicated
`QueryCheckpoint` leaf object into a caller-owned task store. Consume pages
with durable `(task, sequence)` receipts before acknowledgement; unconfirmed
pages repeat until acknowledged. No `pagosc` checkpoint flags or automatic
checkpoint files are introduced in this iteration.

```sh
ctest --test-dir build/debug -R 'QueryCheckpointTest|pagos-query-checkpoint-process' \
  --output-on-failure
```

The process test saves and exits, then resumes the verified evidence with a
new process and different slice budgets, without invoking StageAnalyzer.
C4c1 uses fully resident query tables and a bounded single checkpoint object.
[C4c2a](provenance-task-store-c4c2a.md) adds `TaskStore` for segmented snapshots,
optimistic generation updates, paused-state release and explicit task maintenance.
Use `save` to retain tables or `offload` to release them after atomic publication;
subsequent querying restores all tables. `reclaim_tasks` preserves paused and
AwaitingDelivery tasks by default; AllInactive is an explicit discard operation.
No main-driver query checkpoint flags are added yet.

```sh
ctest --test-dir build/debug \
  -R 'TaskStoreTest|QueryCheckpointTest|pagos-query-task-process' \
  --output-on-failure
```

[C4c2b](provenance-query-paging-c4c2b.md) adds explicit `begin_paged` and
`restore_paged` for active-query paging. Set `QueryPagingConfig` to bound six
append tails plus the decoded cache; segment directories, codec scratch, evidence
leases and detached results are separate. Restore scans and validates all state
without building full query vectors or hash indexes. Storage errors pause with
`storage_failure()`; `resume_with_storage` explicitly rebinds the same namespace
to a writable or larger-quota store. Read-only restoration makes no writes and
pauses when continued traversal needs to seal a segment. The task process test
also checks separate-process paged continuation and unconfirmed delivery replay.
Topology checkpoints, performance acceptance and main-driver flags remain C4c3.

[C4c3a](provenance-topology-checkpoint-c4c3a.md) adds bound `TopologyTask`
checkpoints over verified function evidence, including partial symbolic formula
preparation. Use `TopologyTask(binding, task_id, root)`, acknowledge consumed pages,
then `save(store, expected_generation)` and restore with the exact binding/root.
A read-only store can restore and continue this resident task without writing.
Topology state remains resident and its single checkpoint has codec limits;
there are no topology offload or main-driver checkpoint flags yet.

```sh
ctest --test-dir build/debug \
  -R 'QueryCheckpointTest.Topology|ProvenanceTopology|pagos-topology-checkpoint-process' \
  --output-on-failure
```

[C4c3b](provenance-topology-storage-c4c3b.md) adds explicit topology
`save_segmented` and `offload`; task-store restore detects v3/v4 automatically.
`state_memory()` separates task tables from externally retained formula indexes;
`storage_stats()` describes the last segmented IO operation. Offloaded reads can
pause with `storage_failure()` and retry after repair. Topology restore still
materializes all tables. The independent Release `pagos_query_task_bench` and
`scripts/benchmark_query_tasks.py` compare native paging and topology state;
`pagos-query-task-benchmark-smoke` runs small correctness checks in CTest.

[C4c3c](provenance-query-cli-c4c3c.md) makes the main explain default resumable
without changing small-query text or introducing implicit task files. Explicit
`--query-dir` plus function evidence and one `--evidence-root` enables durable
JSON pages; start generates a fresh task ID, resume/cancel require an ID and
`--evidence-read`. Commit consumer records before `--query-ack=N`. Exit 3 means
saved work or delivery remains. Read-only replay never confirms or advances work;
`reclaim-queries` protects active/resumable tasks unless explicit discard is chosen.
See the linked guide for budgets, precision fields and complete command examples.

```sh
ctest --test-dir build/debug -R 'pagos-query-cli-process|InvocationValueAccess' \
  --output-on-failure
```

[C4d1](provenance-recursive-c4d1.md) exposes `RecursiveSummaryTask` as a C++
experiment over immutable evidence. It computes formal dependencies with sliced
SCC worklists and explicit precision/gap fields; it does not replace the native
CLI, infer Stage, enumerate recursive instances or save solver checkpoints.

[C4d2](provenance-recursive-cli-c4d2.md) adds explicit function-evidence modes
`summaries` and `recursive-summary`. List actual specialization keys, then select
one with `--evidence-summary=NAME` or `--evidence-summary-key=HEX`. Choose
`--evidence-channel=value|effects|both`; `--recursive-max-steps=N|all` limits each
channel. Budget exhaustion returns 3 with `restart_required`, without saving a
continuation. Reads validate the closure/source and never run Stage.

[C4d3a](provenance-recursive-checkpoint-c4d3a.md) adds explicit bound
`RecursiveSummaryTask::checkpoint/restore` for resident execution state. Store
the bytes as `QueryTaskManifest` if using FileStore directly; the codec has no
implicit writes, TaskStore head, offload or delivery acknowledgement. C4d2 CLI
remains a fresh-query interface until persistent delivery is integrated.

```sh
ctest --test-dir build/debug -R 'RecursiveSummary|StageProvenanceQuery|FunctionEvidenceTest.Recursive|pagos-recursive-cli-process' \
  --output-on-failure
ctest --test-dir build/debug -R 'RecursiveCheckpointTest|pagos-recursive-checkpoint-process' \
  --output-on-failure
```
