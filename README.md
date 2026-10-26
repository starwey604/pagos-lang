# Pagos

> **Freeze what is known. Run what remains.**

Pagos is an experimental staged systems programming language for embedded
software. Values and expressions are evaluated at compile time whenever their
dependencies and effects allow it. Runtime computation enters the program only
through explicit runtime sources and propagates through the dataflow from there.

The name comes from Ancient Greek **πάγος** (*pagos*): that which is fixed or
firmly set, and by extension frost or ice. The language uses freezing and
thawing as an explanatory metaphor for Comptime and Runtime values; the formal
model uses binding-time analysis and partial evaluation.

Pagos is currently a research compiler. The first executable vertical slice is
implemented, but the language remains experimental and makes no stability
promise.

## Core idea

```pagos
fn uart_divisor(clock_hz: u32, baud: u32) -> u32 {
    return clock_hz / baud;
}

let clock_hz = 80_000_000;              // inferred Comptime
runtime let baud = external_input();     // explicit Runtime source

let divisor = uart_divisor(clock_hz, baud);
                                           // Runtime residual code;
                                           // clock_hz is still folded in

```

The intended rules are:

- `let` is stage-inferred and is evaluated at compile time when possible.
- `runtime let` introduces a runtime value explicitly.
- Expressions depending on runtime values become runtime expressions.
- `comptime let` requires a compile-time result and reports the dependency path
  that thawed it when the constraint cannot be satisfied.
- Mixed-stage functions are partially evaluated. Pagos emits only the residual
  computation required at runtime.

The goal is not merely more constant folding. Pagos aims to make staging a
predictable language guarantee and to use the same typed language for embedded
configuration, validation, code generation, and ordinary systems code.

## Intended use cases

- Bare-metal and RTOS firmware
- Compile-time protocol tables and register descriptions
- Typed board, peripheral, and driver configuration
- Generation of C headers, device tables, linker scripts, DTB/DTS files, and
  build graphs
- Fixed-hardware Linux experiments where selected platform knowledge can be
  moved out of boot-time discovery
- Interoperation with existing C and, through explicit shims, C++ projects

Pagos does not initially aim to replace Linux, Zephyr, CMake, Kconfig, and
Devicetree at once. The first integration strategy is to generate artifacts
that those ecosystems can already consume.

## Proposed compiler pipeline

Pagos will use its own typed, stage-aware intermediate representations and a
deterministic compile-time evaluator. LLVM is the runtime optimization and code
generation backend; it is not the source of Pagos's staging semantics.

```text
Source
  -> AST
  -> typed HIR with stages and effects
  -> binding-time analysis
  -> compile-time evaluation and partial evaluation
  -> residual SSA MIR
  -> LLVM IR
  -> object code
```

See [Compiler architecture](docs/architecture.md) for the complete design.

## Project documents

- [Project context](docs/project-context.md): origin, decisions, constraints,
  and enough context to resume the project without the original discussion
- [Vision and scope](docs/vision.md): motivation, value proposition, risks, and
  non-goals
- [Core grammar](docs/grammar.md): the syntax baseline for the first compiler
  slice
- [Staging semantics](docs/staging-semantics.md): the normative Comptime/Runtime
  model, effects, residualization, and open questions
- [Stage Model RFC](docs/stage-model-rfc.md): an example-led proposal for stage
  contracts, behavior preservation, explanations, and limited convergence;
  provenance costs and convergence remain research topics
- [Provenance prototype](docs/provenance-prototype.md): representation and query
  experiments and historical measurements
- [Provenance analysis](docs/provenance-analysis.md): task-owned builders,
  immutable snapshots, real stage evidence, and `explain-provenance`
- [Stage Evidence P0](docs/stage-evidence-p0.md): semantic reasons recorded at
  Stage decision points, implementation boundaries, and validation
- [Cache Architecture RFC](docs/cache-architecture-rfc.md): proposed semantic
  query boundaries, stable keys, dependency validation, budget preservation,
  and staged experiments for persistent results and evidence
- [Cache implementation plan](docs/cache-implementation-plan.md): iterations
  for reusable Comptime results, resumable queries, shared summaries, evidence
  snapshots, residual templates, construction spill, and pure detail replay
- [Cache identity C1a](docs/cache-identity-c1a.md): implemented canonical codecs,
  stable definition fingerprints, source anchors, and cross-process validation
- [Cache store C1b](docs/cache-store-c1b.md): filesystem manifests, immutable CAS,
  atomic publication, conflict handling, and process recovery tests
- [Cache scalar C2a](docs/cache-scalar-c2a.md): experimental pure scalar reuse,
  budget replay, current source locations, and independent cache telemetry
- [Cache array C2b](docs/cache-array-c2b.md): closed scalar-array reuse, ordered
  construction/Fuel templates, runtime bounds equivalence, and measurements
- [Provenance queries C3q1/C3q2](docs/provenance-query-c3q.md): resumable
  queries, shared dependency formulas, explicit scopes, and compact topology
- [Provenance snapshots C3a](docs/provenance-snapshot-c3a.md): sectioned module
  evidence, independent source revisions and checked restoration
- [Residual templates C3b](docs/cache-residual-c3b.md): closed scalar HIR reuse,
  mandatory symbolic evidence and logical resource replay
- [Evidence views C4a1](docs/provenance-view-c4a1.md): pinned memory/disk views,
  block reads and native lazy queries
- [Function evidence C4a2](docs/provenance-units-c4a2.md): actual specialization
  identities, local IDs, Imports/SCCs and validated hot closures
- [Function queries C4a3](docs/provenance-function-queries-c4a3.md): lazy SCC
  queries, independent-process CLI and real Stage measurements
- [Evidence construction C4b1](docs/provenance-spill-c4b1.md): segmented builder,
  temporary spill, leases and capacity failures
- [Recorder spill C4b2](docs/provenance-recorder-spill-c4b2.md): Stage/CLI
  integration, native views, auxiliary failures and guarded cache publication
- [Trace sharing C4b3a](docs/provenance-trace-c4b3a.md): persistent diagnostic
  paths and independent real Stage build/query measurements
- [Working sets C4b3b](docs/provenance-working-set-c4b3b.md): lifecycle cleanup,
  real Stage crash recovery and measured large-graph spill peak savings
- [Query checkpoints C4c1](docs/provenance-checkpoint-c4c1.md): bound cross-process
  query state and acknowledged page redelivery
- [Task storage C4c2a](docs/provenance-task-store-c4c2a.md): segmented snapshots,
  atomic heads, paused-state release, quotas and explicit lifecycle cleanup
- [Active query paging C4c2b](docs/provenance-query-paging-c4c2b.md): bounded
  append/read buffers, sorted disk indexes and streamed checkpoint validation
- [Symbolic topology checkpoints C4c3a](docs/provenance-topology-checkpoint-c4c3a.md):
  disk-backed formula preparation, saved cursors and acknowledged page replay
- [Topology state storage C4c3b](docs/provenance-topology-storage-c4c3b.md): segmented
  snapshots, paused-state release and independent query measurements
- [Recursive summary CLI C4d2](docs/provenance-recursive-cli-c4d2.md): actual
  specialization selection, symbolic precision and cycle evidence without Stage.
- [Finite recursive summaries C4d1](docs/provenance-recursive-c4d1.md): sliced
  SCC solving, formal parameter dependencies and explicit precision boundaries.
- [Resumable explain CLI C4c3c](docs/provenance-query-cli-c4c3c.md): explicit task
  pause/resume, acknowledged JSON pages, read-only access and maintenance
- [Semantic examples](docs/semantic-examples.md): executable-design cases with
  expected stages, residual work, and errors
- [Diagnostic conventions](docs/diagnostics.md): stable structure for stage and
  evaluation failures
- [Development guide](docs/development.md): C++26 toolchain, build presets,
  compiler commands, and tests
- [Compiler architecture](docs/architecture.md): the LLVM-based implementation
  plan
- [Roadmap](docs/roadmap.md): incremental milestones and acceptance criteria
- [M3 preparation plan](docs/m3-preparation.md): pre-M3 tasks, dependencies,
  validation gates, and deliberately deferred work

## Status

Milestone 1 is complete. The C++26 reference compiler parses and type-checks the
core language, infers stages, evaluates Comptime expressions, explains Runtime
dependency paths, and emits verified residual LLVM IR for the host target.
The [Milestone 2 core baseline](docs/milestone-2.md) is complete, with total
compiler-memory limits explicitly deferred. Typed, verified residual SSA MIR
sits between partial evaluation and LLVM. Comptime recursion and pure Comptime
specializations are memoized under explicit evaluation limits.
Comptime and Runtime loops, per-call early returns, and immutable fixed-length
arrays are supported. Comptime lookup tables embed as read-only constants, with
bounds-checked Runtime indexing.
Comptime-index reads of mixed arrays retain the selected element's stage while
preserving evaluation of the entire array.
Bulk generation uses `[for i in 0..256 { i * i }]`; bounds can also be Comptime
expressions such as `0..1 << bits`, with concrete lengths per specialization.
Fuel and cumulative construction quotas bound expansion. See the
[computed-bound example](tests/lit/stage/generator-computed.pgs) and
[shape rules](docs/grammar.md#bulk-array-generation).
`u32` bitwise operations and checked shifts support the representative
[CRC-32 example](tests/lit/stage/crc32.pgs): compile-time table generation and
Runtime checksums verified against zlib. See the
[CRC walkthrough](docs/crc32.md) for commands and current limitations.
Minimal named records support immutable scalar fields, field-sensitive staging,
and residual aggregate values. The [record walkthrough](docs/records.md)
demonstrates mixed Comptime/Runtime configuration without a stable-layout promise.
Arrays and records share cumulative member/data-byte construction budgets;
the [resource budget guide](docs/resource-budgets.md) defines accounting,
legacy array limits, and the distinction from host-memory limits.

The [M3 preparation baseline](docs/m3-preparation.md) is complete: pinned
compiler CI, Release measurements, separate semantic types, a private APInt
adapter, target-layout queries, and QEMU RV32 C/assembly environment checks.
Explicit-target LLVM IR, object emission and minimal volatile/MMIO are available;
the complete firmware slice remains M3 work. CI has been exercised locally in the
fixed container, not yet on GitHub Actions.

M3 now includes `u8/u16/u32/u64` and `i8/i16/i32/i64`, typed literals
(`255u8`, `-128i8`), and explicit
conversions (`external_input() as u8`). Arithmetic wraps at the declared width;
Comptime and Runtime checked operations agree. Arrays and records retain element
widths, and construction quotas count logical bytes. See the
[integer rules](docs/grammar.md#core-type-rules) and
[multi-width example](tests/lit/codegen/unsigned-widths.pgs), and the
[signed boundary example](tests/lit/codegen/signed-arithmetic-64.pgs).
Target-sized unsigned `usize` now follows the selected LLVM DataLayout through
checking, Comptime evaluation, MIR, and LLVM emission. It remains distinct from
`u32`/`u64`; see the [RV32/RV64 example](tests/lit/stage/usize-static.pgs).
All CLI commands accept target options. General C ABI support remains pending.

Runtime scalar calls now emit internal functions with typed parameters and
real MIR/LLVM calls; Comptime arguments still specialize their bodies. See the
[nested-call example](tests/lit/codegen/residual-call-scalar.pgs).
Equivalent scalar specializations share one definition within an analysis;
see the [reuse example](tests/lit/codegen/residual-reuse.pgs). The independent
`--max-residual-specializations` limit bounds their count. Aggregate and
non-recursive Comptime-result calls retain the inline path. Scalar direct and
mutual Runtime recursion now reuse active signatures; see the
[recursive example](tests/lit/codegen/runtime-recursion-sum.pgs). Compilation
budgets do not guarantee termination or bound the target stack. These internal
signatures are not C ABI.

The minimal C ABI now provides `extern fn` imports and `export fn` definitions
for `u32/i32` and integer/record pointers, with direct
`emit-obj ... -o output.o` output.
It is validated on x86-64 Linux LP64 and RV32 ELF ILP32 (I/M/A/C). The
[bidirectional example](tests/lit/codegen/c-abi.pgs) runs with C on the host and
as QEMU RV32 firmware; this ABI harness uses C/assembly startup and MMIO.
Other boundary types and targets remain unsupported. See the
[build/run commands](docs/development.md#host-compiler-commands).

Raw `*const T` / `*mut T` pointers now support integer memory loads/stores and
explicit target-`usize` address conversions. Memory access stays Runtime even
at a Comptime address. The [RAM example](tests/lit/codegen/pointers.pgs) runs on
the host and RV32 with C-owned storage. No borrow checker or `unsafe` keyword
is introduced; callers own lifetime/range validity. Local address-taking, allocation,
and RAII remain future slices. See the
[memory contract](docs/grammar.md#raw-pointers-and-memory-milestone-3).

Explicit `volatile *p` / `volatile *p = value;` now preserve device accesses,
with checked natural alignment and native-width restrictions. The
[Pagos MMIO example](platforms/qemu-rv32-virt/mmio.pgs) polls UART, prints a
success/failure message and exits QEMU through its test device without C device
helpers. Startup remains assembly. Volatile is not an atomic operation, CPU
fence or cache policy; see the
[access rules](docs/grammar.md#volatile-access-and-mmio-milestone-3).

`static const` / `static mut` now declare module-private integer storage with
pure Comptime initializers. `&name` yields a relocatable target address; reading
storage remains Runtime. The [global example](tests/lit/codegen/globals.pgs)
tests C access and independent modules. The QEMU UART example also owns its boot
state and detects skipped `.data` copying or `.bss` clearing. See the
[storage rules](docs/grammar.md#global-integer-storage-milestone-3).
Optional `section(".data.boot")` and `align(64)` attributes now control named
ELF data subsections and minimum object alignment. The
[placement example](tests/lit/codegen/global-sections.pgs) checks actual ELF
flags and alignment; final memory regions remain a linker-script decision.
Fixed-length integer globals now support generated Comptime initializers,
Runtime element reads and checked `&name[index]` addresses. The
[array example](tests/lit/codegen/global-arrays.pgs) shares a Pagos-owned buffer
with C on the host and RV32, without whole-array copies or array-pointer types.
Global integer-field records now have declaration-order, target-aligned storage
and Runtime field reads/addresses. The
[record storage example](tests/lit/codegen/global-records.pgs) checks persistent
state and C field-pointer access on the host and RV32; see the
[layout rules](docs/grammar.md#global-integer-field-records). Record pointers now
support `&state`, `(*p).field` and `&(*p).field`, with null checks before field
address calculation. The [pointer example](tests/lit/codegen/record-pointers.pgs)
exchanges C-owned and Pagos-owned records through the existing pointer ABI.
Whole-record copies, local address-taking, bool/nested/pointer-field storage and
record-by-value C calls remain deferred.

## Development

Configure, build, and test the default Clang development preset (LLVM 22 or 23):

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
python3 scripts/check_docs.py
```

See the [development guide](docs/development.md) for GCC, sanitizer, compiler,
formatting, and analysis commands.

## License

ISC License
