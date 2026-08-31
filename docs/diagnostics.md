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

## Testing Rules

Golden tests normalize only platform path separators and explicitly unstable
temporary paths. They do not normalize wording, source positions, dependency
order, or error codes. A diagnostic change therefore requires intentional
fixture review and, when semantics changed, an update to
[Core Semantic Examples](semantic-examples.md).
