# Repository Guidelines

## Project Structure & Module Organization

Pagos is currently a specification-stage language project. `README.md` is the
entry point; design material lives in `docs/`. The dependency-free documentation
checker is `scripts/check_docs.py`, and CI runs it from `.github/workflows/`.
Keep normative semantic rules in `docs/staging-semantics.md` and implementation
plans in `docs/architecture.md`.

The proposed implementation is a C++ compiler with CMake. As code is added,
follow the documented layout: public headers under `include/pagos/`, matching
implementations under `lib/`, the `pagosc` driver under `tools/pagosc/`, and
tests grouped by compiler phase under `tests/`, such as `tests/stage/`. Do not
create empty directories ahead of their milestone.

## Build, Test, and Development Commands

There is no buildable compiler or language test suite yet. For documentation
changes, use:

```sh
python3 scripts/check_docs.py         # validate Markdown and local links
git diff --check                      # catch whitespace and patch errors
git diff -- README.md docs AGENTS.md  # review documentation changes
```

When the planned CMake project lands, document the exact configure, build, and
test commands here rather than assuming conventional defaults.

## Coding Style & Naming Conventions

Write concise Markdown with ATX headings, fenced code blocks with language
tags, and lines wrapped near 80 characters. Use **Pagos** (never `PagOS`),
`pagosc` for the proposed compiler, and `.pgs` for source examples. Formal text
should prefer `Static`, `Runtime`, binding-time analysis, and residual program;
reserve “frozen” and “thawed” for explanatory prose and diagnostics.

For future C++, keep public and implementation paths parallel and name modules
by compiler responsibility (`syntax`, `sema`, `hir`, `stage`, `vm`, `mir`,
`codegen`). Introduce formatter or linter rules with the relevant code.

## Testing Guidelines

Every semantic change should include small positive and negative `.pgs`
examples with unambiguous expected stages or diagnostics. The roadmap requires
snapshot/golden coverage for stage inference and errors; keep fixtures in
`tests/<phase>/`. Test dependency paths and successful residual output. Until a
harness exists, cross-check examples against the staging lattice and record
unresolved behavior explicitly.

## Commit & Pull Request Guidelines

Git history is currently too small to establish a commit convention. Use short,
imperative subjects such as `docs: clarify runtime branch effects`, and keep
each commit focused. Pull requests should state the milestone or design question
addressed, summarize observable behavior changes, link relevant issues, and
include test evidence. For semantic changes, update all affected examples and
call out newly opened or resolved questions; screenshots are only useful for
rendering or diagnostic-output changes.
