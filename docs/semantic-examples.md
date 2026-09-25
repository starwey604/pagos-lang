# Core Semantic Examples

Status: Milestone 0 acceptance corpus. These examples are the source of truth
for future stage and diagnostic golden tests.

Unless stated otherwise, each snippet is an independent `.pgs` source file.
The illustrative prelude provides `read_u32() -> u32` with effect `target.io`
and `build_u32() -> u32` with effect `build.fs`. A declared `build.fs`
capability is assumed except where an example tests its absence.

## Static inference

### 1. Literal binding

```pagos
let answer = 42;
```

Expected: `answer: u32` is Static; no residual operation is emitted.

### 2. Pure arithmetic

```pagos
let divisor = 80_000_000 / 115_200;
```

Expected: `divisor` is the Static `u32` value `694`.

### 3. Static comparison

```pagos
let enabled = 3 < 5;
```

Expected: `enabled: bool` is Static `true`.

### 4. Satisfied static constraint

```pagos
static let table_size = 16 * 4;
```

Expected: `table_size` is Static `64`; the constraint succeeds.

## Runtime roots and propagation

### 5. Explicit runtime root

```pagos
runtime let input = 7;
```

Expected: `input` is Runtime even though its initializer is Static. Residual
storage is not required, and a backend may still fold the constant.

### 6. Direct propagation

```pagos
runtime let input = 7;
let output = input + 1;
```

Expected: `output` is Runtime; residual code contains addition by `1` before
backend optimization.

### 7. Failed static constraint

```pagos
runtime let input = 7;
static let output = input + 1;
```

Expected error: `output` requires Static. The dependency path is
`input -> output`, rooted at the `runtime let`.

### 8. Target effect

```pagos
let sample = read_u32();
```

Expected: `sample` is Runtime because `target.io` can execute only on the
target; the read remains in residual code.

## Calls and specialization

### 9. Fully static call

```pagos
fn double(value: u32) -> u32 {
    return value * 2;
}

let result = double(6);
```

Expected: `result` is Static `12`; no call is residualized.

### 10. Mixed-stage call

```pagos
fn scale(factor: u32, value: u32) -> u32 {
    return factor * value;
}

runtime let sample = 3;
let result = scale(4, sample);
```

Expected: `factor` specializes to `4`; `result` and the multiply are Runtime.
The compile-time evaluator itself never appears in residual code.

### 11. Unused runtime argument

```pagos
fn first(first_value: u32, unused: u32) -> u32 {
    return first_value;
}

runtime let input = 9;
let result = first(5, input);
```

Expected: `result` is Static `5`; stages propagate through actual data use,
not by blindly joining every argument. If the unused argument contains a
Runtime read or loop, that computation still executes before the call body;
the result remains Static. A specialization-cache hit also preserves argument
evaluation.

### 12. Transitive failure path

```pagos
fn scale(factor: u32, value: u32) -> u32 {
    return factor * value;
}

runtime let sample = 3;
let scaled = scale(4, sample);
static let result = scaled;
```

Expected error: the useful path is
`sample -> scale.value -> scaled -> result`.

Recursive calls with fully Static arguments are evaluated and memoized by
function plus typed argument constants. A selected Static base case terminates
normally; an active specialization that calls itself with the same key is an
error. Fuel, recursion-depth, and specialization budgets fail with `E4xxx`
resource diagnostics rather than exhausting the compiler process.

## Control flow

### 13. Static condition

```pagos
runtime let fallback = 9;
let result = if true { 1 } else { fallback };
```

Expected: `result` is Static `1`. The unselected arm is type-checked but does
not thaw the result or produce residual code.

### 14. Runtime condition

```pagos
runtime let select_first = true;
let result = if select_first { 1 } else { 2 };
```

Expected: `result` is Runtime and the conditional is residualized.

### 15. Equal arms under runtime control

```pagos
runtime let condition = true;
let result = if condition { 1 } else { 1 };
```

Expected: `result` is Runtime by language staging rules. LLVM may later fold
the residual conditional without changing what compile-time code may observe.

### 16. Static short-circuit

```pagos
runtime let flag = true;
let result = false && flag;
```

Expected: `result` is Static `false`; the right operand is not observed.

### 17. Runtime short-circuit

```pagos
runtime let flag = true;
let result = true && flag;
```

Expected: `result` is Runtime because the right operand is required.

### 18. Static range loop

```pagos
for index in 0..4 {
    index;
}
```

Expected: the loop executes four times during compilation. Each `index` is a
Static `u32`; the expression leaves no residual work.

### 19. Runtime range loop

```pagos
runtime let end = 4;
for index in 0..end {
    index;
}
```

Expected: the loop and `index` are Runtime and are emitted to residual control
flow. An alias such as `let bound = index;` denotes the same iteration value
and can be passed to a function or used as a nested loop bound. Runtime work
in the body executes per iteration, including expressions whose results are
discarded. An empty or reversed range executes no body operations.

## Effects

### 20. Build effect under static control

```pagos
let value = if true { build_u32() } else { 0 };
```

Expected: with a declared `build.fs` capability, the selected call runs once
during compilation and `value` is Static. The input dependency is recorded.

### 21. Build effect under runtime branch

```pagos
runtime let condition = true;
let value = if condition { build_u32() } else { 0 };
```

Expected error: `build.fs` cannot execute under Runtime control. The diagnostic
points to `condition`, the `if`, and the effectful call.

### 22. Build effect under runtime loop

```pagos
runtime let end = 4;
for index in 0..end {
    build_u32();
}
```

Expected error: a compile-time effect cannot be conditionally repeated by a
Runtime loop.

### 23. Missing capability

```pagos
let value = build_u32();
```

Expected without a declared `build.fs` capability: compilation fails at the
call and names the missing capability. Stage inference does not bypass effect
checking.

## Integer and type rules

### 24. Unsigned overflow

```pagos
let wrapped: u32 = 4_294_967_295 + 1;
```

Expected: `wrapped` is Static `0`, using arithmetic modulo 2^32.

### 25. Literal outside `u32`

```pagos
let too_large: u32 = 4_294_967_296;
```

Expected error: the literal cannot be represented as `u32`; no truncation is
performed.

### 26. Static division by zero

```pagos
let invalid = 10 / 0;
```

Expected error: compile-time evaluation reports division by zero at `/`.

### 27. Possible runtime division by zero

```pagos
runtime let divisor = 2;
let quotient = 10 / divisor;
```

Expected: `quotient` is Runtime. Residual code preserves the defined
division-by-zero trap unless analysis proves the divisor nonzero.

### 28. Branch type mismatch

```pagos
let invalid = if true { 1 } else { false };
```

Expected error: the `if` arms have incompatible types `u32` and `bool`, even
though the condition is Static.

### 29. Runtime-loop early return

```pagos
fn first(end: u32) -> u32 {
    for index in 1..end {
        return index;
    }
    return 9;
}

runtime let end = external_input();
let result = first(end) + 100;
```

Expected: `result` is Runtime. If `end <= 1`, the result is `109`; otherwise it
is `101`. The loop return exits `first`, and the caller still adds `100`.
Without the fallthrough `return 9`, result-completeness checking reports
`E1007`. A `static let result` reports the Runtime control dependency.

### 30. Fixed-length static lookup table

```pagos
fn square(x: u32) -> u32 { x * x }
static let table: [u32; 3] = [square(0), square(1), square(2)];
static let folded = table[2];
let selected = table[external_input()];
```

Expected: `table` is Static `[0, 1, 4]`, `folded` is Static `4`, and `selected`
is Runtime. Residual LLVM contains a read-only table and a bounds-checked
load, not calls to `square`. Index `3` or larger traps. Replacing the Runtime
index with Static `3` reports `E4007` instead.

### 31. Element-sensitive staging and immutable values

```pagos
fn pair(value: u32) -> [u32; 2] { [11, value] }
let values = pair(external_input());
static let first = values[0];
```

Expected: `values` is Runtime but `first` is Static `11`. The input read
executes exactly once, even though `first` selects the known element. This
refines the original whole-array slice; unrelated Runtime elements no longer
thaw a Static-index read. `[u32; 3]` cannot receive this pair: array length is
part of the type. Arrays cannot be compared for equality or mutated yet.

### 32. Array projection respects Runtime boundaries

```pagos
runtime let forced = [11, 22];
runtime let flag = true;
let selected = if flag { [11, 20] } else { [11, 30] };
static let a = forced[0];
static let b = selected[0];
```

Expected: both Static constraints fail with `E2001`. The paths point to
`forced` and `flag`, respectively. Equal element values across Runtime
branches do not make their selection Static. The same rule applies to arrays
selected by Runtime-controlled early returns, and to Runtime indices.

### 33. Selected-element diagnostic provenance

```pagos
runtime let unrelated = external_input();
runtime let selected = external_input();
let table = [unrelated, selected];
static let result = table[1];
```

Expected: `E2001` names `selected`, not `unrelated`, with the path
`selected -> array element 1 -> table -> array index -> result`. Array aliases
and function parameter names remain in this path when present.

### 34. Bulk static lookup generation

```pagos
fn square(i: u32) -> u32 { i * i }
static let table: [u32; 256] = [for i in 0..256 { square(i) }];
let result = table[external_input()];
```

Expected: generation is Static and produces a 256-entry read-only table.
Runtime input `255` returns `65025`; `256` traps. No generator loop or
`square` call remains. `explain-stage` reports 256 reserved array elements
and 1024 logical data bytes. Bounds must be increasing `u32` literals.

### 35. Array construction budgets precede expansion

```pagos
let table = [for i in 0..4294967295 { 1 / 0 }];
```

Expected: default limits report `E4008` before allocating a generated element
vector or evaluating the division. The request exceeds the 65,536-element
and 262,144-byte quotas. `[for i in 0..4 { i }]` needs four slots and 16 bytes;
the boolean form `[for i in 0..4 { i == 0 }]` needs four slots and four bytes.
Several smaller constructions accumulate; unused results and early returns
do not refund reservations. Aliases and cached results are not new arrays.
Full compiler-memory accounting remains separate from these quotas.

### 36. Bitwise operations and checked shifts

```pagos
static let high = 1 << 31;
static let wrapped = 2 << 31;
static let low = high >> 31;
let result = ~external_input() & 255;
```

Expected: `high` is `2147483648`, `wrapped` is zero, and `low` is one.
`result` is Runtime and retains one input read. All bitwise operands must be
`u32`. An analyzed `external_input() << 32` reports `E4009`; with a Runtime
count instead, 32 or greater traps before shifting, even if unused. A skipped
Static branch does not evaluate its invalid shift. Use `(value & 1) != 0`
to test a bit because equality binds more tightly than bitwise AND.

### 37. CRC-32 representative case

The [CRC example](../tests/lit/stage/crc32.pgs) generates a 256-entry table using
eight Static recursive bit steps per entry. The same checksum functions fold
ASCII `123456789` to `3421780262` and process nine Runtime inputs with checked
table loads. Each input contributes its low eight bits. The whole table and
Runtime results agree with Python zlib at external Clang `-O0` and `-O2`.
The [walkthrough](crc32.md) records commands, budgets, and fixed-length scope.

### 38. Named record fields retain independent stages

```pagos
record Config { clock_hz: u32, revision: u32 }
fn keep(config: Config) -> Config { config }
let config = Config(revision: external_input(), clock_hz: 80_000_000);
let alias = keep(config);
static let divisor = alias.clock_hz / 1_000_000;
let result = divisor + alias.revision;
```

Expected: `divisor` is Static `80`, while `result` is Runtime. The input executes
exactly once, even though its initializer precedes the known field and passes
through a call. A failed Static read of `revision` names that field's Runtime
source. A Runtime binding or branch selecting the record keeps every field
read Runtime, even if all candidate field values agree. Record constructors
require all declared fields exactly once and preserve source evaluation order.
See the [record walkthrough](records.md) and
[executable configuration case](../tests/lit/stage/record-config.pgs).

### 39. Arrays and records share construction quotas

```pagos
record Config { value: u32, ready: bool }
let config = Config(value: 7, ready: true);
let flags = [true, false];
```

Expected: two reserved constructions, four aggregate members, and seven logical
bytes; the array-only subset is two elements/two bytes. Shared limits of four
members/seven bytes permit this program. A three-member limit reports `E4010`
on the array without changing either counter set. A four-byte limit rejects the
record before either field initializer executes. Early return does not refund
declared fields; aliases and cached results do not reconstruct values, while
fresh call arguments still consume quota on cache hits. See
[resource budgets](resource-budgets.md) for the exact scope and legacy limits.

### 40. Computed Static generator lengths

```pagos
fn last(n: u32) -> u32 {
    let table = [for i in 0..n { i * i }];
    table[n - 1]
}
static let small = last(2);
static let large = last(4);
```

Expected: `small` is `1`, `large` is `9`; their arrays have independent lengths
two and four. Both bounds evaluate before reservation and body execution.
`last(external_input())` fails with `E2002` and a dependency path through
`last.n`. Returning this table from a function declared `-> [u32; 2]` with
`n == 4` instead fails with `E1008`. Bounds may use Static record fields or
function results, preserving any accompanying Runtime reads/traps/returns.
Runtime branch continuations must agree on concrete shape; Static selection
does not evaluate skipped bounds. See the
[computed table fixture](../tests/lit/stage/generator-computed.pgs) and
[bulk generation rules](grammar.md#bulk-array-generation).

## Acceptance Use

Each example becomes a fixture when its feature enters an implementation
milestone. Milestone 1 covers the core expression, call, stage, type, and LLVM
subset; loop, effect, and richer target cases follow in their scheduled
milestones. Successful cases need stage/HIR snapshots, Runtime cases need
residual IR checks, and failures need stable diagnostic snapshots. Changing an
expected result requires updating the normative semantics in the same review.
