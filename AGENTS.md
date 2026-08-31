# Repository Guidelines

## Project Structure & Module Organization

`README.md` is the entry point and `docs/` contains the language specification,
architecture, roadmap, and development guide. Public C++ headers live under
`include/pagos/`; matching implementations live in `lib/`, grouped by compiler
phase (`syntax`, `sema`, `hir`, `stage`, `vm`, and `codegen`). The `pagosc`
driver is in `tools/pagosc/`. Keep GoogleTest units in `tests/unit/` and
end-to-end `.pgs` fixtures in phase-specific directories under `tests/lit/`.

## Build, Test, and Development Commands

```sh
cmake --preset debug          # configure Clang, LLVM 22, and tests
cmake --build --preset debug  # build libraries, pagosc, and unit tests
ctest --preset debug          # run GoogleTest and lit/FileCheck tests
python3 scripts/check_docs.py # validate Markdown and local links
git diff --check              # catch whitespace and patch errors
```

Use `gcc-debug` for the GCC compatibility build and `asan` for AddressSanitizer
plus UndefinedBehaviorSanitizer. See `docs/development.md` for prerequisites and
individual CLI examples.

## Coding Style & Naming Conventions

The project requires C++26, four-space indentation, and no compiler extensions.
Format C++ with `.clang-format` and check it with `clang-format --dry-run
--Werror`. Use `snake_case` for files/functions, `PascalCase` for types, and
parallel public/implementation paths. Keep syntax nodes free of inferred types
and LLVM details; store semantic facts in side tables and lower through HIR.

Write Markdown with ATX headings, tagged fences, and lines near 80 characters.
Use **Pagos**, `pagosc`, and `.pgs`. Prefer the formal terms `Static`, `Runtime`,
binding-time analysis, and residual program.

## Testing Guidelines

Use GoogleTest for isolated library behavior and lit plus FileCheck for CLI,
diagnostic, HIR, and LLVM IR golden tests. Name unit files `<area>_test.cpp` and
fixtures after behavior, for example `tests/lit/stage/mixed-call.pgs`. Every
semantic change needs focused positive and negative cases. Assert dependency
paths for staging failures and verify successful residual output.

## Commit & Pull Request Guidelines

Git history is currently too small to establish a commit convention. Use short,
imperative subjects such as `docs: clarify runtime branch effects`, and keep
each commit focused. Pull requests should state the milestone or design question
addressed, summarize observable behavior changes, link relevant issues, and
include test evidence. For semantic changes, update all affected examples and
call out newly opened or resolved questions; screenshots are only useful for
rendering or diagnostic-output changes.
