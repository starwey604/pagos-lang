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

Status: core baseline complete, 2026-09-26. The
[baseline summary](milestone-2.md) records implemented capabilities, deferred
work, and regression evidence. Total compiler-memory limits from the original
scope are explicitly deferred for validation with larger system-build workloads.
Cumulative aggregate construction quotas do not satisfy that original memory
requirement. Nested aggregates and dependency-aware effect caching also remain
future work; this baseline does not imply a stable language or systems ABI.

Delivered:

- stage-polymorphic calls and specialization caching;
- static and runtime loops;
- aggregates with field-sensitive staging;
- cross-stage embedding for scalars, arrays, and records;
- SSA residual MIR;
- residual `emit-mir` plus richer `emit-hir` and `explain-stage` reports;
- compile-time fuel, recursion, specialization, and aggregate construction
  limits; total compiler-memory limits remain deferred.

Acceptance criteria:

- CRC/lookup tables can be generated entirely at compile time;
- a runtime index can access a static embedded table;
- reports show which operations remain and why;
- exceeding the configured Static specialization count reports a deterministic
  diagnostic; this is not a guarantee against host-memory exhaustion.

## Milestone 3: Bare-metal systems slice

The [M3 preparation baseline](m3-preparation.md) is complete as of 2026-09-26.
It covers measurement, locally exercised pinned CI, type/value foundations,
target layout, effect interfaces, and the RV32 environment probe. Full M3
language work has begun; remote CI and static-component
LLVM linking are not claimed as verified.

The first M3 slice implements `u8/u16/u32/u64`, typed decimal literals,
explicit integer `as` conversions, and width-aware constants, arrays, records,
construction budgets, MIR checks, and LLVM arithmetic. Existing `u32` programs
retain their behavior. The second slice adds `i8/i16/i32/i64`, negative literals,
wrapping negation, signed comparisons/division/shifts, and signedness-aware
conversions. MIN/-1 division and remainder have explicit checks at both stages.
The third slice adds distinct target-dependent `usize`, target-aware checking
and Static evaluation, and cross-session width consistency checks;
the fourth slice emits scalar residual functions with typed parameters and
direct calls, preserving argument evaluation and early-return boundaries.
The fifth slice reuses equivalent scalar residual specializations within one
analysis, with a separate version budget and hit statistics. Runtime recursion,
aggregate call lowering, C ABI, object emission, memory, and MMIO remain open.

Add the minimum systems features needed for real firmware:

- explicit-width integer types and target `usize`;
- layout-defined records and enums;
- pointers, address spaces, and controlled `unsafe` operations;
- volatile loads/stores and MMIO;
- globals and custom sections;
- C ABI import/export;
- target triples, CPU features, object emission, and linker integration;
- a minimal freestanding core library.

Target order:

- QEMU RV32 `virt` first: single-core bare-metal validation without Linux,
  a vendor HAL, or a physical-board prerequisite;
- Cortex-M or another architecture when it adds portability coverage;
- physical MCUs selected for a concrete project, not required to begin M3.

Simulator baseline acceptance:

- startup, linking, C interoperability, and UART/MMIO tests run automatically
  with explicit output, failure, and timeout checks;
- generated code is compared with C for the same target and optimization mode;
- ELF section sizes and compile costs are recorded; emulator wall time is not
  presented as hardware cycle, timing, or power evidence;
- no host pointer or host layout leaks into target output, and virtual-board
  addresses remain in platform support rather than the compiler core.

Hardware follow-up remains distinct: validate blink/UART, chip-specific startup
and peripherals, flash/RAM usage, and relevant real cycle counts. A simulator
baseline does not imply that any particular CH32, STM32, or other MCU is supported.

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
