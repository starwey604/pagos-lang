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

## Status

Milestone 1 is complete. The C++26 reference compiler parses and type-checks the
core language, infers stages, evaluates Static expressions, explains Runtime
dependency paths, and emits verified residual LLVM IR for the host target.
Milestone 2 is in progress; its first slice adds a typed, verified residual SSA
MIR between partial evaluation and LLVM. Static recursion and pure Static
specializations are memoized under explicit evaluation limits.
Static and Runtime loops, per-call early returns, and immutable fixed-length
arrays are supported. Static lookup tables embed as read-only constants, with
bounds-checked Runtime indexing.

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
