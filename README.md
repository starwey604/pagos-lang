# Pagos

> **Freeze what is known. Run what remains.**

Pagos is an experimental staged systems programming language for embedded
software. Values and expressions are evaluated at compile time whenever their
dependencies and effects allow it. Runtime computation enters the program only
through explicit runtime sources and propagates through the dataflow from there.

The name comes from Ancient Greek **πάγος** (*pagos*): that which is fixed or
firmly set, and by extension frost or ice. The language uses freezing and
thawing as an explanatory metaphor for static and runtime values; the formal
model uses binding-time analysis and partial evaluation.

Pagos is currently a research compiler. The first executable vertical slice is
implemented, but the language remains experimental and makes no stability
promise.

## Core idea

```pagos
fn uart_divisor(clock_hz: u32, baud: u32) -> u32 {
    return clock_hz / baud;
}

let clock_hz = 80_000_000;              // inferred Static
runtime let baud = external_input();     // explicit Runtime source

let divisor = uart_divisor(clock_hz, baud);
                                           // Runtime residual code;
                                           // clock_hz is still folded in

```

The intended rules are:

- `let` is stage-inferred and is evaluated at compile time when possible.
- `runtime let` introduces a runtime value explicitly.
- Expressions depending on runtime values become runtime expressions.
- `static let` requires a compile-time result and reports the dependency path
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
- [Staging semantics](docs/staging-semantics.md): the normative Static/Runtime
  model, effects, residualization, and open questions
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
core language, infers stages, evaluates Static expressions, explains Runtime
dependency paths, and emits verified residual LLVM IR for the host target.
The [Milestone 2 core baseline](docs/milestone-2.md) is complete, with total
compiler-memory limits explicitly deferred. Typed, verified residual SSA MIR
sits between partial evaluation and LLVM. Static recursion and pure Static
specializations are memoized under explicit evaluation limits.
Static and Runtime loops, per-call early returns, and immutable fixed-length
arrays are supported. Static lookup tables embed as read-only constants, with
bounds-checked Runtime indexing.
Static-index reads of mixed arrays retain the selected element's stage while
preserving evaluation of the entire array.
Bulk generation uses `[for i in 0..256 { i * i }]`; bounds can also be Static
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
demonstrates mixed Static/Runtime configuration without a stable-layout promise.
Arrays and records share cumulative member/data-byte construction budgets;
the [resource budget guide](docs/resource-budgets.md) defines accounting,
legacy array limits, and the distinction from host-memory limits.

The [M3 preparation baseline](docs/m3-preparation.md) is complete: pinned
compiler CI, Release measurements, separate semantic types, a private APInt
adapter, target-layout queries, and QEMU RV32 C/assembly environment checks.
Explicit-target LLVM IR and object emission are available; volatile/MMIO and
the complete firmware slice remain M3 work. CI has been exercised locally in the
fixed container, not yet on GitHub Actions.

M3 now includes `u8/u16/u32/u64` and `i8/i16/i32/i64`, typed literals
(`255u8`, `-128i8`), and explicit
conversions (`external_input() as u8`). Arithmetic wraps at the declared width;
Static and Runtime checked operations agree. Arrays and records retain element
widths, and construction quotas count logical bytes. See the
[integer rules](docs/grammar.md#core-type-rules) and
[multi-width example](tests/lit/codegen/unsigned-widths.pgs), and the
[signed boundary example](tests/lit/codegen/signed-arithmetic-64.pgs).
Target-sized unsigned `usize` now follows the selected LLVM DataLayout through
checking, Static evaluation, MIR, and LLVM emission. It remains distinct from
`u32`/`u64`; see the [RV32/RV64 example](tests/lit/stage/usize-static.pgs).
All CLI commands accept target options. General C ABI support remains pending.

Runtime scalar calls now emit internal functions with typed parameters and
real MIR/LLVM calls; Static arguments still specialize their bodies. See the
[nested-call example](tests/lit/codegen/residual-call-scalar.pgs).
Equivalent scalar specializations share one definition within an analysis;
see the [reuse example](tests/lit/codegen/residual-reuse.pgs). The independent
`--max-residual-specializations` limit bounds their count. Aggregate and
non-recursive Static-result calls retain the inline path. Scalar direct and
mutual Runtime recursion now reuse active signatures; see the
[recursive example](tests/lit/codegen/runtime-recursion-sum.pgs). Compilation
budgets do not guarantee termination or bound the target stack. These internal
signatures are not C ABI.

The minimal C ABI now provides `extern fn` imports and `export fn` definitions
for `u32/i32` and integer pointers, with direct `emit-obj ... -o output.o` output.
It is validated on x86-64 Linux LP64 and RV32 ELF ILP32 (I/M/A/C). The
[bidirectional example](tests/lit/codegen/c-abi.pgs) runs with C on the host and
as QEMU RV32 firmware; startup and MMIO still live in C/assembly. Other boundary
types and targets remain unsupported. See the
[build/run commands](docs/development.md#host-compiler-commands).

Raw `*const T` / `*mut T` pointers now support integer memory loads/stores and
explicit target-`usize` address conversions. Memory access stays Runtime even
at a Static address. The [RAM example](tests/lit/codegen/pointers.pgs) runs on
the host and RV32 with C-owned storage. No borrow checker or `unsafe` keyword
is introduced; callers own lifetime/range validity. Address-taking, allocation,
RAII and volatile/MMIO remain future slices. See the
[memory contract](docs/grammar.md#raw-pointers-and-memory-milestone-3).

## Development

Configure, build, and test the default Clang 22 development preset:

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
