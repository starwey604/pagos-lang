# Staging Semantics

Status: normative Milestone 0 baseline for the core grammar. Sections marked as
open questions remain non-normative.

## 1. Stages

Pagos begins with a two-point binding-time lattice:

```text
Static <= Runtime

join(Static, Static)   = Static
join(Static, Runtime)  = Runtime
join(Runtime, Runtime) = Runtime
```

`Static` means the compiler can produce the value during compilation under the
permitted compile-time effects. `Runtime` means the residual target program
must produce or carry the value.

Stage inference is semantic, not an optimization report. Later LLVM passes may
remove Runtime operations, but that does not permit Pagos compile-time code to
observe a Runtime value.

Frozen and thawed are explanatory aliases:

| Formal term | Diagnostic metaphor |
| --- | --- |
| `Static` | frozen |
| `Runtime` | thawed |
| runtime dependency propagation | thaw path |

The stage is a property of values and operations. It is distinct from
mutability: a static value may be mutated by the compile-time evaluator, and an
immutable binding may contain a runtime value.

## 2. Binding forms

Core surface forms:

```pagos
let a = expression;          // infer stage
static let b = expression;   // require Static
runtime let c = expression;  // introduce or require Runtime
```

An unqualified binding does not mean "always Static." It requests the most
static valid result. If the initializer depends on runtime data, the binding is
Runtime.

`static` adds a constraint rather than an optimization hint. Failure is a
compile error and should include the shortest useful runtime dependency path.

`runtime` is a semantic boundary, not an anti-optimization annotation. LLVM may
still optimize the residual expression under normal as-if rules, but Pagos may
not use its value during compile-time evaluation.

The initializer of `runtime let` is checked normally and the resulting binding
is always Runtime, even when the initializer is a literal. It need not force a
memory allocation in residual code.

## 3. Expression propagation

For a strict pure primitive operation, the result stage is the join of the
operand stages. Static operands are evaluated immediately. Runtime operations
are emitted into residual IR with static operands materialized as constants.
`&&` and `||` first evaluate their left operand; a Static left operand that
determines the result prevents the right operand from thawing the expression.

Analysis should be value- and field-sensitive:

```pagos
let config = {
    clock_hz: 80_000_000,        // Static
    runtime revision: read_id()  // Runtime
};
```

The runtime `revision` field must not thaw `clock_hz` or unrelated device
topology. Exact record syntax remains undecided.

Fixed-length array construction retains each element's stage. The array as a
whole is Static only when every element is Static, but a Static index selects
that element's stage and dependency path rather than joining unrelated
elements. This precision survives aliases, call parameters, and direct returns.
For example, `[11, external_input()][0]` is Static `11`; the input read still
executes exactly once. A Static value need not be free of residual work.

Explicit `runtime let` array boundaries and Runtime control selecting an array
(including per-call return joins) keep every projection Runtime. A Runtime
index also keeps its read Runtime, even for identical elements. Projection
does not look through these boundaries or compare branch values. Array and
index evaluation preserve all residual work in source order, including traps
and early returns. See [Core Grammar](grammar.md#fixed-length-arrays-milestone-2)
for syntax, length rules, and checked bounds behavior.

The compiler converts control flow to SSA-like values before final stage
propagation. A merge is Runtime when a reachable incoming value is Runtime or
when Runtime control selects between incoming values.

## 4. Functions and partial evaluation

Functions are stage-polymorphic unless their signature imposes stage
constraints. A call is analyzed from the values actually used by the selected
path through the function body; an unused Runtime argument does not by itself
thaw the result.

Value stage and residual execution are separate: a call can produce a known
Static constant while still requiring Runtime work. Statements execute in
source order and arguments are evaluated left-to-right before the callee body,
including unused arguments. Discarding a value must preserve required reads,
checked-arithmetic traps, and residual loops. A binding denotes one evaluation;
aliases and repeated uses do not repeat that evaluation. Short-circuit operands
and unselected branches remain conditional.

```pagos
fn scale(factor: u32, value: u32) -> u32 {
    return factor * value;
}

let factor = 4;
runtime let sample = 3;
let result = scale(factor, sample);
```

The call specializes on `factor` and emits a residual multiply for `sample`.
The entire function does not become Runtime simply because one argument is
Runtime. Backend strength reduction, such as replacing multiplication by a
shift, is outside the staging semantics.

The compiler memoizes specializations by function identity, static arguments,
target configuration, and relevant effect dependencies. It must enforce a
specialization budget to prevent accidental code-size explosion.

The current implementation caches a call only when every argument and the
result are Static and evaluating the specialized body has no residual work.
Argument evaluation is preserved separately, including on cache hits. Its key
contains function identity and the typed argument constants; target
configuration and effect dependencies join the key when those features land.
Static recursion is permitted when selected
Static branches change the arguments and terminate within the configured fuel,
depth, and specialization limits. Re-entering an active key is a recursive
specialization cycle. Runtime recursion is not residualized yet and is rejected
explicitly.

## 5. Control flow

### Static condition

Both branches must parse, resolve names, and type-check. Only the selected
branch is executed and contributes effects or residual code. The result stage
is the selected branch's stage.

### Runtime condition

The conditional is emitted to residual IR. Both reachable branches must be
valid runtime code. Its value is Runtime because branch selection depends on
Runtime control, even if both branch values are Static. Compile-time effects
may not be conditionally performed under Runtime control.

In the following example, `build_u32()` denotes an intrinsic with the
compile-time `build.fs` effect:

```pagos
runtime let enabled = true;

let value = if enabled { build_u32() } else { 0 };
// error: compile-time effect under Runtime control
```

Static loops execute in the compile-time evaluator. Runtime loops residualize.
For the core range form, a loop is Static when both bounds are Static; each
iteration variable is a new Static binding. If either bound is Runtime, the
loop, its iteration variable, and values selected by its control are Runtime.
Compile-time effects are forbidden inside a Runtime loop. Compile-time loops
are subject to fuel and memory limits.

`return` exits the enclosing function from any nesting depth. Under Static
control the evaluator stops that path immediately. Under Runtime control it
emits an edge to the current call's result merge; it does not return from the
caller. Only paths that continue can execute later statements, operands, or
loop increments. A zero-trip loop reaches the function's fallthrough result.
If Runtime control selects an early return versus a later result, the call
result is Runtime even when the individual returned values are Static. Its
dependency path includes the controlling source and `return control`.

Short-circuit expressions follow the same control rule: a statically skipped
right operand performs no effect, while a right operand selected by a Runtime
left operand is under Runtime control.

Literal-range array generators expand in ascending index order with a fresh
Static index per iteration. The body follows the same staging, effect-order,
and enclosing-function return rules as a Static loop; only scalar continuation
values become array elements. A Runtime body does not make the length dynamic.
Static selection skips unselected generators; Runtime branches are both
analyzed and charged against cumulative array quotas. Construction inside a
Runtime loop is charged when analyzed, not on each target execution. Bounds,
reservation rules, and defaults are specified in
[Bulk array generation](grammar.md#bulk-array-generation).

## 6. Effects and capabilities

Stages alone cannot determine when an operation is legal. The initial semantic
effect categories are:

```text
pure        deterministic computation with no external effects
build.fs    declared build-time filesystem access
build.env   declared build-time environment access
target.io   target-side I/O such as MMIO, sensors, and interrupts
unsafe      raw pointers, inline assembly, and unchecked operations
```

Possible later effects include allocation, concurrency, process execution, and
network access.

`build.fs` and `build.env` calls record exact dependencies for incremental and
content-addressed builds. Network access should be disabled by default. External
commands belong to a declarative build action graph rather than unrestricted
compile-time process spawning.

## 7. Cross-stage persistence

A static value used by residual code must be representable in the target
program. This requires a language concept tentatively called `Embed`:

- fixed-width scalars, arrays, enums, and layout-known records are normally
  embeddable;
- strings and slices require a defined target representation and lifetime;
- function references require a residual symbol or a defined closure layout;
- compiler AST nodes, file handles, capability tokens, and host pointers are
  not embeddable;
- target relocations must be created explicitly rather than manufacturing host
  addresses.

The compiler may place embedded values in immediates, `.rodata`, generated
tables, or target-specific sections.

## 8. Target and integer semantics

Compile-time arithmetic must agree with target execution. The evaluator uses
explicit target-width representations rather than native host C++ arithmetic.

The Milestone 1 core has only `bool` and `u32`. Decimal literals are exact,
untyped non-negative values until context supplies `u32`; without another
context, a literal binding defaults to `u32`. A literal that does not fit is a
compile error. Unary `-` is outside the core grammar until signed types are
specified.

`u32` addition, subtraction, and multiplication wrap modulo 2^32 at both
stages. Division or remainder by zero is an error during static evaluation and
a defined runtime trap in residual code. Comparisons produce `bool`. The
evaluator must implement these rules explicitly and must not inherit the host
C++ integer model.

Milestone 2 adds strict `u32` bitwise `&`, `|`, `^`, and complement `~`.
`<<` shifts left modulo 2^32; `>>` is logical (zero-filling), not arithmetic.
Both operands are `u32` and evaluate left-to-right. A Static shift count of
32 or more reports `E4009` when the operation is analyzed, even if the left
operand is Runtime. A Runtime count is checked against 32 before execution;
invalid counts trap, including in discarded expressions. Counts are never
implicitly masked. Explicit Runtime boundaries remain opaque to staging.
The LLVM backend must not execute an out-of-range shift or add overflow/exact
flags that contradict these rules. Static and residual evaluation agree at
counts 0 and 31 and on discarded high bits.

Signed integers, floating point, implicit conversions, and `usize` are outside
the core grammar. Before introduction, each must define overflow and target
layout behavior; `usize` must follow the selected target data layout even when
evaluated on the host.

MMIO, volatile access, interrupts, and target assembly are always residual
target effects.

## 9. Diagnostics and observability

Required tools and messages include:

```text
pagosc explain-stage source.pgs
pagosc emit-hir source.pgs
pagosc emit-mir source.pgs
```

Example constraint failure:

```text
error: static binding `table` was thawed
  --> board.pgs:18:12

runtime source introduced here:
  --> board.pgs:7:28
      runtime let revision = soc.read_revision();

dependency path:
  revision -> select_calibration -> table
```

Diagnostics follow the stable presentation and path-selection rules in
[Diagnostic Conventions](diagnostics.md). Stage failures must name the
constraint, the first Runtime source, and the shortest useful dependency path.

## 10. Open questions

- Are explicit stage annotations permitted on function parameters and return
  types, and what is their syntax?
- How does mutable local state interact with compile-time evaluation?
- What memory and ownership model should the systems language use?
- Which generic and reflection facilities are necessary for the first useful
  embedded configuration library?
- Can runtime aggregate fields remain independently staged in the type system,
  or is field sensitivity only an analysis property?
- What memory accounting and default resource limits should releases promise?
- How are floating-point reproducibility and target-specific behavior exposed?
- Which compile-time effects are stable enough to include in cache keys?
