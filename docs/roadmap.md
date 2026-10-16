# Development Roadmap

The roadmap is organized around vertical proofs. Dates are intentionally absent;
each milestone should finish with measurable artifacts before scope expands.

## Milestone 0: Semantic specification

Status: baseline complete. The normative grammar, staging rules, diagnostic
contract, and 28 acceptance examples are now recorded. Any change to an
expected result must update the relevant rule and example together.

Deliverables:

- grammar sketch for the core expression language;
- normative definitions of `let`, `comptime let`, and `runtime let`;
- Comptime/Runtime propagation rules;
- rules for pure calls, Comptime and Runtime branches, loops, and diagnostics;
- a decision on integer overflow and target-width arithmetic;
- at least 20 small examples with expected stage results and errors.

Acceptance criteria:

- examples have unambiguous expected behavior;
- dynamic control flow and effects are covered, not only arithmetic;
- open questions are explicitly recorded rather than silently guessed.

## Milestone 1: First residual program

Status: baseline complete. The C++26 compiler implements the vertical slice
below with typed HIR, explainable stage paths, pure Comptime evaluation, verified
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
- a violated `comptime` binding reports its runtime source;
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
- Comptime and Runtime loops;
- aggregates with field-sensitive staging;
- cross-stage embedding for scalars, arrays, and records;
- SSA residual MIR;
- residual `emit-mir` plus richer `emit-hir` and `explain-stage` reports;
- compile-time fuel, recursion, specialization, and aggregate construction
  limits; total compiler-memory limits remain deferred.

Acceptance criteria:

- CRC/lookup tables can be generated entirely at compile time;
- a Runtime index can access a Comptime embedded table;
- reports show which operations remain and why;
- exceeding the configured Comptime specialization count reports a deterministic
  diagnostic; this is not a guarantee against host-memory exhaustion.

### Stage Model follow-up

The [Stage Model RFC](stage-model-rfc.md) develops the semantic foundation for
the typed board configuration experiment: stage contracts, behavior
preservation, explainable results and residual work, and proposed limited
convergence. The current staging rules remain the implementation baseline.

The [Provenance analysis](provenance-analysis.md) now publishes immutable
evidence built by each Stage analysis and supports all-source/effect queries.
Real compilation costs, recursive fixed points, and cache boundaries need
further measurement. It preserves existing stage results; convergence needs
an explicit update to
the semantics, examples, and execution tests. This work introduces no new stage
keywords or business-code stage reflection.

The [Stage Evidence P0 iteration](stage-evidence-p0.md) moves evidence
production to the semantic decision points for explicit Runtime boundaries,
selection, member projection, and conservative recursive completion. It keeps
current language behavior and graph queries, with snapshot rule queries and tests for
wrappers, inline instances, cached summaries, and snapshot ownership.

Before expanding P1 Comptime explanations, the
[Cache Architecture RFC](cache-architecture-rfc.md) defines the proposed
boundaries for persistent results, dependencies, evidence, and logical budgets.
Its C1-C4 sequence starts with stable keys and independent codecs, then a pure
Comptime disk-cache experiment, followed by residual templates/evidence and
incremental queries. The [implementation plan](cache-implementation-plan.md)
splits C1-C3 into stable keys, filesystem/CAS storage, pure scalar reuse, array
generation reuse, module evidence snapshots, and residual templates. mmap,
compression, and database indexes require later measurements.
[C1a](cache-identity-c1a.md) is complete: canonical codecs, checked-AST identities,
source anchors, and driver-supplied query context. [C1b](cache-store-c1b.md)
is complete: filesystem manifests, a CAS, atomic publication and recovery.
[C2a](cache-scalar-c2a.md) is complete: experimental pure scalar reuse with
budget and provenance equivalence, explicit cache modes, and physical telemetry.
[C2b](cache-array-c2b.md) is complete: closed scalar arrays/generators, ordered
resource templates, runtime bounds equivalence and independent-process
measurements. [C3a snapshots](provenance-snapshot-c3a.md) and
[C3b scalar residual templates](cache-residual-c3b.md) are complete.

[C3q1/C3q2](provenance-query-c3q.md) are complete: resumable memory queries,
shared dependency formulas, explicit decision/contract scopes and paginated
compact topology. C3a now persists module evidence and independent source
revisions; C3b reuses closed scalar HIR with mandatory evidence and resource
validation. [C4a1](provenance-view-c4a1.md) provides manifest reference reads,
memory/disk evidence views, pinned block lifetimes and native lazy queries.
[C4a2](provenance-units-c4a2.md) captures actual specialization identities,
freezes local IDs and Imports/SCCs and validates hot root closures/decisions.
[C4a3](provenance-function-queries-c4a3.md) completes lazy function queries,
CLI reads without Stage, and real large-function measurements. C4a is complete.
[C4b1](provenance-spill-c4b1.md) adds the independent segmented/spill builder;
[C4b2](provenance-recorder-spill-c4b2.md) integrates Recorder/CLI, auxiliary
failures and publication guards. [C4b3a](provenance-trace-c4b3a.md) shares
RuntimeTrace prefixes and adds independent Stage build/query measurements.
[C4b3b](provenance-working-set-c4b3b.md) completes retained-work accounting,
working-table release, real Stage crash/cleanup tests and large-graph build-peak
acceptance. Spill remains explicit; small tasks show no stable net benefit.
[C4c1](provenance-checkpoint-c4c1.md) adds bounded query checkpoints with strict
snapshot/source binding and acknowledged page redelivery across processes.
[C4c2a](provenance-task-store-c4c2a.md) adds segmented snapshots, atomic task
heads, paused-state capacity release, quotas and lifecycle cleanup. C4c2b/C4c3
still need active-query paging/indexes, measurements and the main explain CLI.
C4b-C4e cover construction spill, persistent query checkpoints,
finite-domain recursive explanations and pure detail replay. These extensions
are explanation capabilities, not new Stage inference rules. Query work limits,
recorded coverage, loaded detail, and UI expansion remain separate; evidence
storage does not require enumerating every path or dynamic recursive call.

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
and Comptime evaluation, and cross-session width consistency checks;
the fourth slice emits scalar residual functions with typed parameters and
direct calls, preserving argument evaluation and early-return boundaries.
The fifth slice reuses equivalent scalar residual specializations within one
analysis, with a separate version budget and hit statistics. The sixth slice
supports scalar direct and mutual Runtime recursion through active signatures
and bounded recursive specialization growth. The seventh slice adds direct
object emission and minimal `extern fn`/`export fn` C interoperability for
`u32/i32` on x86-64 Linux LP64 and generic RV32 ELF ILP32 (I/M/A/C). Real
Pagos/C firmware runs on QEMU with Clang and GCC C callers/callees; startup
and MMIO remain in the C/assembly harness. The eighth slice adds non-owning
integer pointers, C pointer signatures, ordinary RAM loads/stores and `usize`
address conversions, without `unsafe` or mandatory borrowing. Aggregate call
lowering, additional ABI types/targets and allocation remain open. The ninth
slice adds explicit volatile accesses, natural-alignment checks and native
integer-width limits. Pagos-native UART/test-device firmware now runs in QEMU
without C device helpers; fences, DMA, interrupts and real hardware remain open.
The tenth slice adds private integer globals with pure Comptime initializers,
symbolic global addresses and default ELF data sections. Pagos-owned startup
state detects missing data copying or BSS clearing in QEMU. The eleventh slice adds
named ELF data subsections and minimum object alignment, with verified section
flags, BSS-zero constraints and linked RV32 startup coverage. Arbitrary sections,
retained/uninitialized memory and fixed-address placement remain open.
The twelfth slice adds global fixed-length integer arrays, pure Comptime generated
initializers, Runtime element reads and checked element addresses. Host and RV32
C interoperation uses existing integer pointers; no aggregate calling convention
is implied. The thirteenth slice adds global integer-field records with
declaration-order, target-aligned storage layout, pure Comptime initializers,
Runtime field reads and integer field pointers. Host/RV32 C layout and alias
checks cover padded sizes/offsets and startup data/BSS, without aggregate call
lowering. The fourteenth slice adds nominal record pointers, whole-global record
addresses and scalar field projections through pointers. Global and indirect
fields share null-checked address lowering; C exchanges record pointers on the
host/RV32, including aliases, recursive helpers and pointer results. Whole-record
copies, whole-array addresses, local address-taking, nested/pointer-field storage,
bool storage, external globals and aggregate-by-value C conventions remain open.

Add the minimum systems features needed for real firmware:

- explicit-width integer types and target `usize`;
- layout-defined records and enums;
- programmer-managed raw pointers, explicit memory rules and address spaces;
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

The proposed [semantic cache protocol](cache-architecture-rfc.md) supplies
shared storage and dependency infrastructure. Build actions remain separate
from semantic queries and Stage evidence; capability inputs must record exact
dependencies before their results can be persisted.

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

- Comptime CRC and trigonometric lookup-table generation;
- register field and peripheral configuration;
- protocol packet parsing with Comptime schema and Runtime bytes;
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
