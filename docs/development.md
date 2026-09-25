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
- Clang 22 and a host C linker/runtime for executing generated LLVM IR;
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

Bound compile-time work with options placed anywhere before or after the
command:

```sh
build/debug/pagosc --max-fuel=100000 check source.pgs
build/debug/pagosc --max-recursion-depth=64 explain-stage source.pgs
build/debug/pagosc --max-specializations=512 emit-mir source.pgs
build/debug/pagosc --max-array-elements=4096 --max-array-bytes=16384 check source.pgs
```

`explain-stage` ends with deterministic analysis statistics for fuel,
specializations, cache hits, maximum recursion depth, reserved array elements,
and reserved array data bytes. Array limits are cumulative construction
quotas, not process-memory caps. Defaults are 65,536 elements and 262,144 bytes;
`u32` costs four bytes and `bool` one logical byte per planned element.
Reservations precede expansion and are not refunded on early return. Cache
hits and aliases do not reconstruct arrays. A zero array quota permits scalar
code, but rejects any analyzed array construction.

`external_input()` is the temporary Milestone 1 runtime-source intrinsic. It
returns `u32` and lowers to a declaration of `pagos_external_input` in LLVM IR.

For the fixed-length array slice, inspect the folded and residual examples:

```sh
build/debug/pagosc emit-hir tests/lit/stage/static-array.pgs
build/debug/pagosc emit-mir tests/lit/stage/array-lookup.pgs
build/debug/pagosc emit-llvm tests/lit/stage/array-lookup.pgs
```

The lookup example embeds one read-only table and retains a checked Runtime
index. Arrays currently support nonempty one-dimensional `u32`/`bool` values;
Static-index reads preserve individual element stages through aliases and
direct calls. Inspect `tests/lit/stage/array-projection.pgs` for a Static
result that still retains an unrelated Runtime input read. Nested arrays and
element mutation remain deferred.

Generate a table with `[for i in 0..256 { i * i }]`. Literal bounds determine
the array type before evaluation. Each iteration consumes fuel as well as the
normal body cost; literals, generators, and Runtime array constructions share
the array quotas. Inspect the complete 256-entry lookup example:

```sh
build/debug/pagosc explain-stage tests/lit/stage/generated-table.pgs
build/debug/pagosc emit-llvm tests/lit/stage/generated-table.pgs
```

`E4008` means the next array reservation would exceed a quota; its label shows
the request, already-reserved amounts, and both configured limits. Increase
limits deliberately. AST, HIR, cache copies, and LLVM memory are not counted.

## Test and Check

The [CRC-32 walkthrough](crc32.md) demonstrates a complete generated table,
Static check vector, Runtime checksum, and budget failures. Its differential
test requires only the existing Python standard library (`zlib`) and host Clang;
no additional package is needed. It checks external `-O0` and `-O2` execution.

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

The lit substitution `%run_residual %s <result|trap> [input ...]` compiles
emitted LLVM IR with a small host harness. It checks the result or trap and
the exact input-read sequence, rejecting missing or extra reads. Execution
has a timeout to catch broken loop backedges. The harness lives in
`tests/lit/Inputs/`; it exercises alias identity, evaluation order, short
circuiting, loop boundaries, discarded results, and specialization-cache hits.
