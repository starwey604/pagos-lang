# Diagnostic Conventions

Status: normative presentation contract for the first compiler slice.

## Goals

Diagnostics must explain the source-level cause, not expose compiler data
structures. Every error has a concise primary message, one primary source span,
ordered supporting labels, and an actionable note when the programmer can
resolve the problem. Wording and ordering must be deterministic for golden
tests.

## Text Format

```text
error[E2001]: static binding `table` was thawed
  --> board.pgs:18:12
   |
18 | static let table = select_calibration(revision);
   |            ^^^^^ requires a Static value

runtime source introduced here:
  --> board.pgs:7:13
   |
 7 | runtime let revision = read_u32();
   |             ^^^^^^^^

dependency path: revision -> select_calibration -> table
help: remove `static` or remove the Runtime dependency
```

Use `error`, `warning`, and `note` consistently. The first line must stand on
its own in compact logs. Paths are relative to the invocation directory when
possible; line and column numbers are one-based. Source excerpts use the
original spelling and never include terminal color in snapshots.

## Required Error Classes

The initial implementation reserves stable categories rather than finalizing a
large numbering scheme:

- `E1xxx`: lexical, syntax, name-resolution, and type errors;
- `E2xxx`: stage constraints and Runtime dependency errors;
- `E3xxx`: compile-time effects and missing capabilities;
- `E4xxx`: compile-time evaluation failures and resource limits;
- `E5xxx`: residualization and target-lowering failures.

Once published in a release, a code must retain its meaning. New diagnostics
within a category take the next unused number.

## Stage Explanations

A failed `static` constraint must include:

1. the binding or expression that required Static;
2. the earliest explicit Runtime source or runtime-only effect;
3. the shortest useful dependency path between them;
4. the call parameter or control edge when it disambiguates the path.

Path selection uses fewest explanation edges. Ties are broken by source-file
path, then byte offset, so identical input produces identical output. Collapse
compiler-generated nodes that do not help the programmer, but preserve function
names and parameter names. Runtime control is written as, for example,
`condition -> if selection -> result`.

`pagosc explain-stage` uses the same explanation graph for successful Runtime
values. This prevents normal diagnostics and inspection tools from disagreeing.

## Evaluation and Effect Failures

Static division by zero, invalid arithmetic, recursion limits, fuel exhaustion,
and memory limits point to the operation plus the source-level evaluation stack.
Effect errors name the effect (`build.fs`, for example), the missing capability
or Runtime control dependency, and the enclosing call.

Resource-limit messages must report the configured limit and consumed amount.
They must not present resource exhaustion as an internal compiler error.

The current evaluator assigns `E4003` to unsupported recursion or an active
specialization cycle, `E4004` to fuel exhaustion, `E4005` to recursion depth,
and `E4006` to the specialization budget. The corresponding CLI controls are
`--max-fuel`, `--max-recursion-depth`, and `--max-specializations`.

`E4007` reports an analyzed Static array index outside the fixed length,
including the index and length in its label. Runtime indices instead trap
before memory access. Array shape, element, and index-type errors use `E1008`;
invalid literal lengths use `E1005`.

`E4008` reports array construction quota exhaustion before reserve or element
evaluation. Its label includes the requested element count, bytes per element,
reserved totals, and configured `--max-array-elements` / `--max-array-bytes`
limits. Only the first exhausted reservation is reported. Quotas count logical
construction data, not peak host memory. Out-of-range bound literals and analyzed
empty/reversed generator ranges use `E1005`; range labels include start and end.
Invalid bound/body types use `E1008`. Computed array shape mismatches also use
`E1008`, with concrete expected and actual lengths at bindings, arguments,
returns, function tails, or Runtime joins. Generator bounds and iterations
participate in existing fuel limits.

`E2002` reports an analyzed generator start/end that is not Static. It identifies
the bound expression, labels its Runtime source, and appends `array generator
start` or `array generator end` to the dependency path. Making a generated
array Runtime does not relax this fixed-shape requirement. Bound evaluation
failures precede outer-array reservation, so its counters are unchanged;
constructors evaluated inside a bound keep their own reservations.

`E4009` reports an analyzed Static shift count of 32 or more, with the count
expression as its primary span. This also applies with a Runtime left operand.
Runtime counts instead trap before shifting. Bitwise operand-type errors use
`E1008`; unselected Static branches do not produce evaluation failures.

`E4010` reports shared aggregate construction quota exhaustion before member
allocation or initializer evaluation. The primary label gives requested `bool`
and `u32` member counts, reserved members/bytes, and both configured shared
limits. Diagnostic arithmetic never needs an unchecked count sum or byte
product. Arrays first check their legacy quotas (`E4008`); failed reservations
change neither counter set. Only the first construction-budget failure is
reported across arrays and records. Increase `--max-aggregate-members` and/or
`--max-aggregate-bytes` deliberately; these are not host-memory limits.

## Testing Rules

Minimal records use `E1003` for unknown record types, `E1004` for duplicate
declarations/fields/initializers or record/function name collisions, and
`E1008` for field/type/shape errors and unsupported record equality. Field
projection failures use `E2001` and trace the selected field's Runtime source,
not the first unrelated Runtime field. Explicit Runtime/control boundaries
instead remain the source of the opaque record's projection.

Golden tests normalize only platform path separators and explicitly unstable
temporary paths. They do not normalize wording, source positions, dependency
order, or error codes. A diagnostic change therefore requires intentional
fixture review and, when semantics changed, an update to
[Core Semantic Examples](semantic-examples.md).
