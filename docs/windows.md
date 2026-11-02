# Windows Host Development

## Scope

W1-W4 provide native Windows x64 development for the C++23 compiler.
MSVC and clang-cl have Ninja Release and RelWithDebInfo configurations.
Windows hosts can use the existing LLVM X86, RISCV, and ARM backends.

Host unit, process, lit, and native execution regressions run on Windows.
W4 admits the minimal `extern fn` / `export fn` C ABI on Windows x64 MSVC
COFF, with actual MSVC and Clang C interoperability and static archives.
DLLs, platform libraries, and Pagos source-level debugging are later work.
Linux/QEMU remains the embedded execution baseline.

## Dependencies

Install Visual Studio's **Desktop development with C++** workload, including
the x64 MSVC tools and Windows SDK, plus CMake 3.30 or newer and Ninja.
The MSVC host requires 19.43 or newer and a bounded C++23 mode. MSVC before
19.52 uses `/std:c++23preview`; newer toolsets use `/std:c++23`. Configuration
checks the selected mode and `std::expected`, `std::print`, and `ios::noreplace`.
clang-cl uses CMake's Clang C++23 mode and the Microsoft standard library.

Warnings remain target-scoped and follow `PAGOS_WARNINGS_AS_ERRORS`. MSVC
uses `/W4`; only targets consuming LLVM headers suppress C4702, following
LLVM's own policy for optimizer warnings in instantiated ADT templates.

Supply an LLVM 22/23 development SDK containing headers, component libraries,
and `lib/cmake/llvm/LLVMConfig.cmake`, with X86, RISCV, and ARM enabled.
A compiler-only LLVM installer is not sufficient. For clang-cl, put the
selected Clang 22/23 tools first on `PATH`; the Clang bundled with Visual
Studio may be a different version.

Supply GoogleTest with its CMake config for unit tests. Also supply the
dependencies required by the chosen LLVM SDK, such as zlib, static zstd, and
the Visual Studio DIA SDK. Discovery uses config packages and the SDK's
imported targets; `CMAKE_PREFIX_PATH` can point to their installed prefix.
The build handles zlib packages exporting `ZLIB::ZLIBSTATIC` as well as
`ZLIB::ZLIB`. DIA is normally found through the developer shell's
`VSINSTALLDIR`; use `MSVC_DIA_SDK_DIR` if necessary.

Dependencies must match x64, the selected CRT, and the SDK configuration.
The Windows presets select static CRT (`MultiThreaded`, `/MT`) and LLVM
component libraries. This matches the LLVM 23.1.4 Release SDK used for local
validation. Static LLVM libraries and static CRT are separate choices: an
SDK using `/MD` needs corresponding dependencies and an explicit override,
not just a different linker flag. A CRT explicitly conflicting with one
selected by the SDK is rejected.

Use Release or RelWithDebInfo with a Release SDK. Ordinary Debug requires
matching Debug LLVM, CRT, and STL dependencies; mixing it with the Release
SDK is rejected. W1 supports single-configuration generators so build type,
effective flags, and compiler identity remain unambiguous.

Prepare dependencies explicitly, through installed packages or a separately
prepared SDK. Normal CMake configuration never downloads or builds LLVM or
missing dependencies. Preserve exact package/toolset versions for CI and
reproducible local investigations; this step does not introduce source
dependency fallbacks.

## Configure, Build, and Test

For the pinned Windows test environment, run this explicit preparation step:

```powershell
./scripts/prepare_windows.ps1
$tools = Get-Content -Raw build/windows-tools/toolchain.json | ConvertFrom-Json
. ./ci/windows_environment.ps1
Enter-PagosWindowsEnvironment $tools.llvm
cmake --preset windows-msvc-release `
    "-DLLVM_DIR=$($tools.llvm)/lib/cmake/llvm" `
    "-DCMAKE_PREFIX_PATH=$($tools.dependencies)" `
    "-DPython3_EXECUTABLE=$($tools.python)" `
    "-DPAGOS_LIT=$($tools.lit)" `
    "-DPAGOS_FILECHECK=$($tools.filecheck)"
cmake --build --preset windows-msvc-release --parallel 4
ctest --preset windows-msvc-release
```

Preparation downloads the complete LLVM 23.1.4 x64 SDK, installs three host
dependencies from a fixed vcpkg commit, and creates a Python environment with
hash-pinned lit. It builds upstream FileCheck's matching command-line source
against the SDK's existing `LLVMFileCheck` library. SDK/source checksums and
versions are in [the lock file](../ci/windows-toolchain.json), using the
[official LLVM release](https://github.com/llvm/llvm-project/releases/tag/llvmorg-23.1.4).
The source retains its upstream license notice. These are explicit test-tool
preparation operations; ordinary project configuration still only discovers
installed packages. The script never updates Visual Studio or system settings.

To reuse matching installed packages, pass `-LLVMRoot C:/SDK/llvm-23.1.4`
and `-DependencyPrefix C:/SDK/pagos-host-x64-mt` to the preparation script.
`-WorkDirectory` selects its download/build cache. Existing cached downloads
are checksum-checked, and existing vcpkg checkouts must match the lock file.

For manually prepared dependencies, use the following environment setup.
Open **Developer PowerShell for VS 2026** configured for x64. When initializing
it manually, use the installed Visual Studio tools script, for example:

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/Tools/Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64
$utf8 = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8
```

Set the paths to your already installed SDK and matching host dependencies:

```powershell
$llvmRoot = 'C:/SDK/llvm-23.1.4'
$hostDependencies = 'C:/SDK/pagos-host-x64-mt'
$testTools = 'C:/SDK/pagos-test-tools'
$env:PATH = "$llvmRoot/bin;$env:PATH"

cmake --preset windows-msvc-release `
    "-DLLVM_DIR=$llvmRoot/lib/cmake/llvm" `
    "-DCMAKE_PREFIX_PATH=$hostDependencies" `
    "-DPAGOS_FILECHECK=$testTools/FileCheck.exe"
cmake --build --preset windows-msvc-release --parallel 4
ctest --preset windows-msvc-release
```

For clang-cl, replace the preset with `windows-clang-cl-release` in all three
commands. The `windows-msvc-relwithdebinfo` and
`windows-clang-cl-relwithdebinfo` presets add host debug information while
retaining a Release-compatible CRT/STL configuration. `where.exe cl` and
`where.exe clang-cl` help confirm the selected tools.

Keep machine paths in explicit arguments or a local `CMakeUserPresets.json`,
which is ignored by Git. There are no project-injection helpers or copied
source trees in this workflow. Use a new build directory after updating the
toolset or switching compilers, architectures, or dependency configurations.

Set both console encodings before the first configure and keep them set for
builds. CMake and MSVC can otherwise disagree about localized `/showIncludes`
bytes, causing Ninja to miss header changes. Configuration checks the prefix
and a Unicode header name. After changing encodings in an existing build,
configure with `--fresh` and rebuild with `--clean-first` to replace stale
objects and dependency records. An English Visual Studio language pack is
not required.

## Test Selection and Current Limits

The Windows presets enable all three GoogleTest suites plus process and lit
regressions. Linux presets retain their full regression suites as well.
`BUILD_TESTING=OFF` omits all testing dependencies and targets.

For a unit-only development build, explicitly deselect the other suites:

```powershell
cmake --preset windows-msvc-release `
    -DPAGOS_ENABLE_PROCESS_TESTS=OFF -DPAGOS_ENABLE_LIT_TESTS=OFF
```

lit requires Python, lit, FileCheck, Clang, ld.lld, llvm-nm, and llvm-objdump.
Process regressions require Python, Clang and `llvm-ar`; Windows also uses
`cl` for its C interoperability matrix. These options never auto-disable
themselves when a tool is missing. Some Windows LLVM development archives omit
FileCheck; the preparation step above supplies a matching binary.

lit uses its internal shell on both hosts and the actual `pagosc` target file,
including `.exe`. Source/tmp substitutions are already shell-quoted; write
RUN lines using `%s` and `%t.suffix` without adding another layer of quotes.
Use `%FileCheck`, `%diff_text`, and `%assert_absent` for portable assertions.
Use `%pagosc_host` for maintenance commands or tests of the real default target.
Text comparisons normalize LF/CRLF only; protocol fixtures remain byte-exact.
Per-case temporary stems are short hashes, leaving room for the nested
CAS/task digest namespaces within Windows path limits.
Python source fixtures explicitly write UTF-8/LF so source-span shifts have
the same byte meaning on both hosts.

Frontend/MIR/IR goldens use an explicit x86-64 Linux semantic target.
Runtime helpers use the real host target and assert its exact trap status
(POSIX SIGILL or Windows illegal-instruction exception). Features distinguish
`native-entry`, `minimal-c-abi`, `elf-native`, and `symlinks`. Native C ABI,
pointer, volatile, ordinary global storage and separate-module tests run on
both x64 hosts. Tests requiring ELF sections/native shared libraries or
unavailable symlink permissions are reported as unsupported.
`tests/lit-results.json` in each build directory records individual outcomes
and unsupported reasons; CI preserves it alongside CTest's JUnit report.

W2 resolves the seven original Windows unit failures. Ordinary cleanup,
unknown namespaces, and non-regular directory rejection always run. Three
separate symlink cases skip only for unavailable permissions/capabilities;
unexpected fixture errors still fail. The working-memory census retains
allocated empty-map buckets and compares release against the pre-analysis
baseline, rather than assuming every standard library releases to zero.

The process suite includes `pagos-host-paths-process`, covering Unicode and
spaces in executable, current directory, source, object output, semantic
cache, snapshots, spill, and query tasks. It also checks cache fallback,
explicit load failure, read-only behavior, and failed output preservation.
Other process regressions cover real checkpoint/replay restoration, writer
races, killed owners, spill reclamation, failed output, and cancellation.
Windows output failure uses a closed pipe; Linux retains `/dev/full`.
Replay cancellation accepts Windows console break as well as Ctrl+C, using
the existing exit 130/unavailable contract. The separate POSIX stop/continue
query test returns CTest skip code 77 on Windows. Ordinary query control and
blocked-output lease checks still run there.

Benchmark smoke checks run on both hosts. Windows omits unavailable wait4/RSS
counters explicitly; performance measurements still require the Linux path.

## Native C Interoperability (W4)

The admitted Windows target normalizes to x86-64, MSVC environment, COFF,
and 64-bit pointers; use `--target=x86_64-pc-windows-msvc` explicitly when
cross-compiling. The native Windows default selects that ABI. The existing
Linux, RV32 and Cortex-M33 admissions retain their previous rules. Windows
x86, MinGW/Cygwin environments, and non-COFF variants remain rejected at the
C boundary; LLVM backend availability alone does not admit another ABI.

The boundary accepts `u32/i32` and pointers to supported integer/record types.
Use C `uint32_t`, `int32_t`, and corresponding pointer types. A `usize` field
or pointee maps to `uintptr_t`, which is 64 bits under Windows LLP64; do not
substitute C `long`. Records require matching natural field layout and are
exchanged through pointers. By-value `usize`, other integer widths, records,
arrays, floating point, varargs and direct C++ ABI remain outside this subset.

Generate an object and link the existing bidirectional C example with MSVC
from the configured x64 developer environment:

```powershell
./build/windows-msvc-release/pagosc.exe -O2 emit-obj `
    tests/lit/codegen/c-abi.pgs -o build/c-abi.obj
cl /nologo /TC /std:c11 /MT /D_CRT_SECURE_NO_WARNINGS `
    tests/lit/Inputs/c_abi.c /Fobuild/c-abi-caller.obj /Febuild/c-abi.exe `
    /link build/c-abi.obj
./build/c-abi.exe
```

Use `llvm-ar rcs build/pagos-abi.lib build/c-abi.obj` to create a static
archive, then pass that `.lib` instead of the object after `/link`. These
are ordinary external toolchain steps; `pagosc` does not embed a linker.
Public function names retain their source spelling. Boundary-only modules
omit `pagos_main`, so separately compiled Pagos objects can import each other.

Both MSVC-built and clang-cl-built compilers run the same native Clang C
harnesses and an additional 15-case MSVC C matrix. The matrix covers raw IR/
object emission, external O2 and native O2, direct object/static archive
linking, signed values, ten-argument calls, mixed scalar/pointer stack slots,
recursion, record layouts, initialized/zero/readonly storage, pointer-sized
fields, aliasing, volatile effects and exact trap status. A cross-target
fixture checks COFF machine/symbol information and non-leaf unwind records
from any compiler host. Generator replay now tests both explicit C effects
and builtin input effects on both hosts with the same assertion paths.

Explicit `section(...)` metadata remains ELF-only; ordinary globals and
alignment work in COFF. DLL import/export annotations, Windows SDK bindings,
exceptions crossing Pagos frames and source-level debugging remain deferred.

## Continuous Regression

[Windows CI](../.github/workflows/windows.yml) runs MSVC Release for PRs and
both MSVC/clang-cl Release on pushes, manual runs, and the weekly schedule.
It uses the Windows Server 2025 / VS 2026 runner image, prepares the locked
tools, builds before running CTest, and saves configuration, test, and tool
metadata. The runner image/toolset itself can roll forward; its actual image
and VC tool versions are recorded. Remote CI runs require pushing the change.

The existing Linux Clang/GCC, sanitizer, and QEMU jobs remain in place.
CI also uses a lit working directory containing Unicode/spaces, and a separate
process regression covers executable/source paths with apostrophes as well.

## Native Paths and Logical Module Names

The driver obtains the Unicode Windows command line through LLVM and uses
UTF-8 at CLI/LLVM boundaries, with native UTF-16 filesystem paths internally.
Source names in diagnostics and JSON use UTF-8. Libraries accept native
`std::filesystem::path` values; callers passing UTF-8 bytes can use the
helpers in `pagos/source/path.h`.

Protocol module names remain byte strings. They require nonempty components
separated by `/`, reject `.`, `..`, NUL, backslash, colon, and absolute or
noncanonical paths, and never use native code-page conversion. Existing
non-UTF-8 wire names retain the baseline admission and round-trip behavior.

## Compiler Identity and Protocols

The conservative compiler build identity includes CMake module content,
C++ standard mode, frontend variant, and selected CRT, in addition to existing
source/configuration inputs. Changing the host toolchain can invalidate cache
reuse as intended. W1-W4 do not change C1-C4 wire formats, hash domains,
versions, fixed fixtures, or old-format rejection rules; see the
[protocol baseline](cache-protocol-baseline.md).
