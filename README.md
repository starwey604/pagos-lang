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

Pagos is currently a research project. The syntax and semantics below are a
design sketch, not a working compiler or a stability promise.

## Core idea

```pagos
fn uart_divisor(clock_hz: u32, baud: u32) -> u32 {
    return clock_hz / baud;
}

let clock_hz = 80_000_000;              // inferred Static
runtime let baud = uart.read_baud();     // explicit Runtime source

let divisor = uart_divisor(clock_hz, baud);
                                           // Runtime residual code;
                                           // clock_hz is still folded in

static let crc_table = make_crc_table(); // must remain Static
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
- [Compiler architecture](docs/architecture.md): the LLVM-based implementation
  plan
- [Roadmap](docs/roadmap.md): incremental milestones and acceptance criteria

## Status

Pagos is at the specification stage. The Milestone 0 semantic baseline is
complete; the next deliverable is a small vertical slice proving that explicit
runtime sources can produce correct and explainable residual LLVM IR.

## Development

The repository does not contain a compiler yet. Validate documentation and
local links with the dependency-free check used by CI:

```sh
python3 scripts/check_docs.py
```

## License

ISC License
