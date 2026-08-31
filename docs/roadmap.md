# Development Roadmap

The roadmap is organized around vertical proofs. Dates are intentionally absent;
each milestone should finish with measurable artifacts before scope expands.

## Milestone 0: Semantic specification

Status: baseline complete. The normative grammar, staging rules, diagnostic
contract, and 28 acceptance examples are now recorded. Any change to an
expected result must update the relevant rule and example together.

Deliverables:

- grammar sketch for the core expression language;
- normative definitions of `let`, `static let`, and `runtime let`;
- Static/Runtime propagation rules;
- rules for pure calls, static and runtime branches, loops, and diagnostics;
- a decision on integer overflow and target-width arithmetic;
- at least 20 small examples with expected stage results and errors.

Acceptance criteria:

- examples have unambiguous expected behavior;
- dynamic control flow and effects are covered, not only arithmetic;
- open questions are explicitly recorded rather than silently guessed.

## Milestone 1: First residual program

Status: baseline complete. The C++26 compiler implements the vertical slice
below with typed HIR, explainable stage paths, pure static evaluation, verified
host LLVM IR, and GoogleTest plus `lit`/`FileCheck` coverage.

Implement a host compiler in C++ with:

- source manager and diagnostics;
- lexer and parser;
- integers, booleans, immutable bindings, functions, calls, and `if`;
- a minimal type checker;
- typed HIR with stage constraints;
- a compile-time interpreter for pure expressions;
- residual arithmetic and control flow;
- LLVM IR emission for the host target.

Demonstration:

```pagos
fn scale(factor: u32, value: u32) -> u32 {
    return factor * value;
}

let factor = 4;
runtime let input = external_input();
let output = scale(factor, input);
```

Acceptance criteria:

- `factor` is evaluated at compile time;
- runtime input remains symbolic;
- emitted LLVM IR contains no compile-time evaluator machinery;
- a violated `static` binding reports its runtime source;
- stage tests are snapshot/golden tests.

## Milestone 2: Explainable partial evaluation

Status: in progress. The first slice introduces typed residual SSA MIR,
verification, stable `emit-mir` output, and MIR-only LLVM lowering. Loop,
aggregate, cross-stage embedding, caching, and resource-limit work remains.

Add:

- stage-polymorphic calls and specialization caching;
- static and runtime loops;
- aggregates with field-sensitive staging;
- cross-stage embedding for scalars, arrays, and records;
- SSA residual MIR;
- residual `emit-mir` plus richer `emit-hir` and `explain-stage` reports;
- compile-time fuel, recursion, memory, and specialization limits.

Acceptance criteria:

- CRC/lookup tables can be generated entirely at compile time;
- a runtime index can access a static embedded table;
- reports show which operations remain and why;
- specialization limits fail predictably instead of exhausting the host.

## Milestone 3: Bare-metal systems slice

Add the minimum systems features needed for real firmware:

- explicit-width integer types and target `usize`;
- layout-defined records and enums;
- pointers, address spaces, and controlled `unsafe` operations;
- volatile loads/stores and MMIO;
- globals and custom sections;
- C ABI import/export;
- target triples, CPU features, object emission, and linker integration;
- a minimal freestanding core library.

Initial targets:

- one Cortex-M board used by the project owner;
- one RISC-V target when it adds coverage rather than just another demo.

Acceptance criteria:

- blink and UART examples run on hardware;
- generated code is compared with a C implementation;
- flash, RAM, compile time, and relevant cycle counts are recorded;
- no host pointer or host layout leaks into target output.

## Milestone 4: Typed board configuration experiment

Choose one real robotics controller and model:

- clocks;
- memory regions;
- GPIO, UART, SPI/I2C, and CAN peripherals;
- interrupts and priorities;
- driver enablement and dependencies;
- one runtime board revision or product variant.

Generate at least two existing-ecosystem artifacts, for example:

- a C header and static initialization table;
- a Zephyr overlay and configuration fragment;
- a DTS/DTB and linker script.

Acceptance criteria:

- invalid pin, clock, interrupt, and dependency combinations fail with typed
  source diagnostics;
- the runtime variant thaws only the required fields/selection logic;
- output integrates into an existing C/C++ or Zephyr build without a fork;
- maintenance cost is compared honestly with the original configuration.

## Milestone 5: Reproducible build model

Add:

- capability-scoped filesystem and environment APIs;
- exact dependency recording;
- declarative build actions;
- a standalone action executor;
- content-addressed caching;
- deterministic artifact serialization;
- a build explanation command.

Acceptance criteria:

- undeclared inputs are rejected or clearly reported;
- identical declared inputs reproduce identical generated outputs;
- changing one configuration input invalidates the smallest correct subgraph;
- external commands are never hidden inside ordinary compile-time evaluation.

## Milestone 6: Linux fixed-platform experiment

Only begin after the embedded configuration experiment succeeds.

Select one fixed-hardware Linux target and measure the baseline using kernel and
userspace boot instrumentation. Generate or embed platform information through
an integration layer without initially changing generic Linux driver semantics.

Measure separately:

- bootloader and image loading;
- kernel decompression;
- Devicetree scan/unflattening;
- individual initcalls and driver probes;
- firmware and storage waits;
- root filesystem and userspace startup.

Acceptance criteria:

- before/after measurements identify where time and size changed;
- the experiment does not claim unrelated boot improvements;
- loss of hardware flexibility is documented as a tradeoff;
- upstream compatibility costs are recorded.

## Deferred work

- self-hosting;
- package registry and remote dependency resolution;
- direct arbitrary C++ ABI integration;
- language-level concurrency and async model;
- general-purpose garbage collection;
- full Linux/Zephyr build-system replacement;
- MLIR adoption;
- ORC JIT acceleration for compile-time execution;
- a stable language specification or compatibility promise.

## Benchmark set

Maintain paired Pagos and C/C++ implementations for:

- static CRC and trigonometric lookup-table generation;
- register field and peripheral configuration;
- protocol packet parsing with static schema and runtime bytes;
- board revision selection;
- compile-time driver graph validation;
- a small real firmware application.

Track:

- compile wall time and peak memory;
- residual MIR and LLVM IR size;
- object/text/rodata/data/bss size;
- runtime cycles or latency where meaningful;
- specialization count;
- diagnostic quality through golden tests.
