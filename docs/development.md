# Development Guide

## Toolchain Contract

The Milestone 1 reference compiler is built as C++26. Supported development
configurations use Clang 22 or GCC 16 with LLVM 22.x. Code may use C++26
features implemented by both compilers; avoid relying on a single compiler's
experimental extension.

Required tools and libraries are:

- CMake 3.30 or newer and Ninja;
- LLVM 22 development headers, libraries, tools, and CMake configuration;
- GoogleTest for C++ unit tests;
- Python 3, LLVM `lit`, and `FileCheck` for language-level golden tests;
- `clang-format` and `clang-tidy` for local quality checks.

LLVM packages differ across distributions. CMake loads `LLVMConfig.cmake`,
checks `LLVM_VERSION_MAJOR`, and prefers the monolithic `LLVM` target when the
package enables `LLVM_LINK_LLVM_DYLIB`. Component libraries are the fallback.
Override discovery with `-DLLVM_DIR=/path/to/lib/cmake/llvm` when necessary.

## Configure and Build

The default preset uses Clang and produces `build/debug/pagosc`:

```sh
cmake --preset debug
cmake --build --preset debug
```

Use `gcc-debug` for the second supported host compiler, `release` for optimized
builds, and `asan` for AddressSanitizer plus UndefinedBehaviorSanitizer.

## Run the Compiler

```sh
build/debug/pagosc check source.pgs
build/debug/pagosc emit-hir source.pgs
build/debug/pagosc explain-stage source.pgs
build/debug/pagosc emit-mir source.pgs
build/debug/pagosc emit-llvm source.pgs
```

`external_input()` is the temporary Milestone 1 runtime-source intrinsic. It
returns `u32` and lowers to a declaration of `pagos_external_input` in LLVM IR.

## Test and Check

```sh
ctest --preset debug
ctest --preset gcc-debug
python3 scripts/check_docs.py
clang-format --dry-run --Werror $(rg --files -g '*.h' -g '*.cpp')
clang-tidy --quiet -p build/debug $(rg --files lib tools -g '*.cpp')
```

GoogleTest covers C++ units such as lexing, source locations, evaluator
arithmetic, and MIR verification. `lit` and `FileCheck` cover complete `.pgs`
programs, stable diagnostics, typed HIR, residual SSA MIR, and LLVM IR. When a
restricted environment cannot run LeakSanitizer, pass
`ASAN_OPTIONS=detect_leaks=0` only for that run; do not disable leak checks in
normal CI.
