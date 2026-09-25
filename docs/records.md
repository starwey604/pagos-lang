# Minimal Record Staging

This Milestone 2 slice generalizes per-element array staging to named scalar
fields. It does not establish a systems-level layout or foreign ABI.

## Declare, construct, and project

```pagos
record Config { clock_hz: u32, ready: bool }
fn keep(config: Config) -> Config { config }

let config = Config(ready: external_input() != 0, clock_hz: 80_000_000);
let alias = keep(config);
static let divisor = alias.clock_hz / 1_000_000;
let result = divisor + (if alias.ready { 3 } else { 0 });
```

Declarations are module-level, including forward references. Records are
nominal: two distinct names are different types even with identical fields.
Constructors use named arguments, without ambiguity with `if`/loop blocks;
ordinary function calls remain positional. Every field must appear exactly
once. Field order in the constructor may differ from declaration order, but
initializers always execute in their written order.

## Staging and execution

`config` is Runtime, yet `clock_hz` and `divisor` are Static. Passing through
aliases, parameters, and direct returns preserves this field precision.
Reading a Static field still preserves all initializer reads, checks, and
early returns. Repeated projections never repeat the constructor's evaluation.
The example reads one input and returns `83` for nonzero input or `80` for zero.

Explicit `runtime let` records, Runtime conditional selection, and Runtime
call-return joins keep projections Runtime. The compiler does not inspect
equal values behind these boundaries. A failing Static constraint reports the
selected field's dependency path rather than an unrelated field's input.

Fully Static records become typed constants. Cache keys include the record
name and typed fields in declaration order; constructor spelling order does
not affect equality. Cache hits must still execute argument initializer work.
Bodies with residual work remain ineligible for pure-result caching.

## Inspect and validate

The [executable configuration example](../tests/lit/stage/record-config.pgs)
adds positive HIR/MIR/LLVM checks and exact input-count execution tests:

```sh
build/debug/pagosc explain-stage tests/lit/stage/record-config.pgs
build/debug/pagosc emit-hir tests/lit/stage/record-config.pgs
build/debug/pagosc emit-mir tests/lit/stage/record-config.pgs
build/debug/pagosc emit-llvm tests/lit/stage/record-config.pgs
python3 tests/lit/Inputs/run_residual.py build/debug/pagosc clang \
  tests/lit/stage/record-config.pgs 83 1
```

MIR has record construction and field projection operations, with nominal
types and verified scalar field kinds. LLVM represents residual records as
struct values, including control-flow phis, not compiler-side addresses.
Tests cover initializer order, traps, branch joins, loop returns, caching,
invalid declarations/types/fields, and opaque Runtime boundaries.

## Deliberate limits

Records must be nonempty, with `u32`/`bool` fields only. Nested records, array
fields, arrays of records, methods, mutation, equality, generics, packing, and
stable C ABI are not supported. An aggregate-valued implicit entry returns zero.
Fuel, recursion depth, and specialization limits apply normally. Shared
`--max-aggregate-members` / `--max-aggregate-bytes` quotas reserve every declared
field before initializer evaluation, including constructors that later return
early. The older array quotas remain array-only. Constant/cache copies are not
counted; see [resource budgets](resource-budgets.md). No process-memory bound is
promised.
