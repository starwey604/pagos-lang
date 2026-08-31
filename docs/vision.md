# Vision and Scope

## Vision

Pagos is a staged systems programming language in which values are evaluated at
compile time unless runtime data or runtime-only effects make that impossible.
Programmers identify the boundary of reality -- sensors, interrupts, input,
mutable hardware state, and other runtime sources -- while the compiler
propagates that boundary and emits the residual program.

The intended experience is:

- ordinary code expresses both computation and configuration;
- compile-time execution does not require a separate template language;
- runtime work is explicit, inspectable, and measurable;
- a failed static requirement explains exactly which runtime dependency caused
  it;
- abstractions can disappear without hiding how or why they disappeared.

## Why this is more than constant folding

Conventional optimizing compilers already fold constants and eliminate dead
code. Pagos is not justified by reproducing those optimizations.

Its value must come from language-level guarantees and composition:

- functions that are stage-polymorphic and partially evaluated;
- explicit runtime roots and explainable dependency propagation;
- compile-time construction of typed data structures, tables, and device
  graphs;
- controlled persistence of those structures into target memory;
- build inputs and effects tracked as part of language semantics;
- configuration and systems code sharing the same types and libraries.

## Target domains

The first target is constrained embedded software where hardware and product
configuration are largely known before deployment:

- microcontrollers and bare-metal firmware;
- Zephyr and other RTOS applications;
- robotics controllers and real-world embodied intelligence devices;
- protocol stacks, register maps, and generated lookup tables;
- product families with mostly static topology and a small runtime variant key.

Linux is a later integration target, especially fixed-hardware products. Pagos
may generate built-in platform tables, DTBs, linker inputs, or other artifacts,
but replacing the Linux device model is not an initial goal.

## Product shape

Pagos should grow in layers:

1. **Language core** -- staging, types, functions, control flow, and residual
   code.
2. **Systems core** -- explicit-width integers, memory layout, pointers,
   volatile access, address spaces, and C ABI.
3. **Configuration libraries** -- typed board and device descriptions that can
   generate existing ecosystem artifacts.
4. **Build graph library and executor** -- declared actions, hermetic inputs,
   content-addressed caching, and incremental execution.

The build executor should remain separate from compile-time evaluation.
Compile-time code constructs a declarative build graph; the executor performs
external commands. This separation is required for caching and reproducibility.

## Non-goals for the initial project

- Replacing Linux, Zephyr, CMake, Kconfig, and Devicetree simultaneously
- Transparent compatibility with arbitrary C++ templates and ABI details
- Claiming that every static specialization improves speed or size
- Allowing unrestricted compile-time network and process access
- Using LLVM optimization as a substitute for a defined staging model
- Designing a production package manager before the core semantics work
- Self-hosting before the reference implementation is stable

## Risks

### Code-size explosion

Specialization and loop unrolling can trade runtime work for flash usage and
instruction-cache pressure. Pagos needs specialization budgets and reports, not
an assumption that more compile-time work is always better.

### Compile-time resource usage

Termination is undecidable. Compile-time execution needs configurable fuel,
memory limits, recursion diagnostics, and deterministic failure behavior.

### Stage surprises

Implicit inference can become mysterious. `pagosc explain-stage`, dependency
paths in diagnostics, and textual HIR/MIR dumps are essential product features,
not optional debugging tools.

### Cross-compilation mismatch

Compile-time evaluation runs on the host but reasons about the target. Integer
width, floating-point behavior, layout, endianness, address spaces, and pointer
provenance must follow target semantics. Host handles and pointers must never be
embedded accidentally.

### Scope

A language, compiler, build system, HAL ecosystem, and Linux experiment are
individually large projects. Progress depends on vertical slices and real
measurements rather than broad incomplete subsystems.

## Success criteria

Pagos has demonstrated its core value when it can:

1. compile representative mixed-stage programs to residual code comparable to
   hand-written C;
2. explain why every residual operation remained at runtime;
3. reject a violated `static` constraint with a useful dependency path;
4. generate and consume an embedded configuration artifact in an existing
   project;
5. report compile time, code size, RAM, and runtime performance against a fair
   C/C++ baseline.
