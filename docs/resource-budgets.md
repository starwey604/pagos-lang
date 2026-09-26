# Aggregate Construction Budgets

Pagos bounds compilation work with deterministic logical quotas. These are not
process RSS limits, allocation counts, target RAM estimates, or a sandbox.

## Shared limits and legacy compatibility

| Option | Default | Scope |
| --- | --- | --- |
| `--max-aggregate-members` | 65,536 | Array elements plus record fields |
| `--max-aggregate-bytes` | 262,144 | Logical payload of all those members |
| `--max-array-elements` | 65,536 | Array elements only |
| `--max-array-bytes` | 262,144 | Array logical payload only |

Each integer costs width/8 bytes; each `bool` costs one. Record field names, padding,
and container overhead do not contribute. Arrays whose continuation type is
`never` conservatively cost four bytes per element; record costs use declared
field types. Zero shared limits allow scalar programs and unused declarations,
but no analyzed aggregate construction.

The existing array flags retain their syntax and array-only scope; they are not
aliases for shared limits. Both sets must permit an array. Programs that raise
legacy limits beyond shared defaults must now raise the shared limits too.
Array-only rejection uses `E4008` and is checked before shared rejection
(`E4010`). Neither failure partially changes statistics.

## What is charged

Each analyzed constructor reserves its complete shape before allocating its
member vector or evaluating any initializer/generator body. Reservations are
cumulative and never refunded, including unused values, early returns, and
later evaluation failures. Only the first construction-budget failure is
reported per analysis; counters and exhaustion state reset for the next one.

Generator bounds evaluate before that generator's reservation. Their ordinary
fuel/call costs and any nested constructors are charged first. If a bound
returns or fails, the outer array has no reservation; a returning body instead
keeps its full reservation. Computed lengths cannot bypass either set of array
quotas. Literal bounds now also consume ordinary expression fuel.

- Both Runtime branch bodies are analyzed and charged; an unselected Static
  branch is not.
- Static loops and generators charge each construction they analyze. Runtime
  loops charge the analyzed body, not every target iteration.
- Aliases, projections, and cached results do not reconstruct values.
- Fresh argument constructors are charged before a cache lookup, even on a hit.
- Constructor calls inside an initializer consume additional quota, even when
  only a scalar field escapes the inner construction.

`explain-stage` appends `aggregate-constructions`, `aggregate-members`, and
`aggregate-bytes`. These count successful reservations, not necessarily
completed values. Existing array statistics are a subset, not a second charge.

## Example

```pagos
record Config { value: u32, ready: bool }
let config = Config(value: 7, ready: true);
let values = [2, 3];
```

This reserves two constructions, four members, and 13 bytes: five for the record
and eight for the array. The executable counterpart can be checked with:

```sh
build/debug/pagosc --max-aggregate-members=4 --max-aggregate-bytes=13 \
  explain-stage tests/lit/stage/aggregate-budget-mixed.pgs
build/debug/pagosc --max-aggregate-members=3 \
  check tests/lit/stage/aggregate-budget-mixed.pgs
```

The second command fails with `E4010`, reporting two already-reserved members
and a request for two more. Validation covers mixed payload widths, exact limits,
zero limits, huge generators, atomic reservation, early returns, branching,
loops, aliases, and cache hits.

## Remaining memory work

These limits do not account for AST/HIR nodes, record schemas, constant copies,
specialization key/result copies, LLVM allocations, or allocator overhead.
Bounding those requires separate accounting and ownership decisions. Raising
construction limits can still increase compiler memory and residual code size;
the existing fuel, recursion-depth, and specialization limits remain independent.
