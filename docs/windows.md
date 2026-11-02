# Windows Host Development

## Scope

W1 provides native Windows x64 build entry points for the C++23 compiler.
MSVC and clang-cl have Ninja Release and RelWithDebInfo configurations.
Windows hosts can use the existing LLVM X86, RISCV, and ARM backends.

This is host build support, not a validated Windows C ABI or complete Windows
regression support. Explicit Windows `extern fn` / `export fn` remain rejected.
Target admission, DLLs, platform libraries, and Pagos source-level debugging
are later work. Linux/QEMU remains the embedded execution baseline.

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

Open **Developer PowerShell for VS 2026** configured for x64. When initializing
it manually, use the installed Visual Studio tools script, for example:

```powershell
& 'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/Tools/Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64
```

Set the paths to your already installed SDK and matching host dependencies:

```powershell
$llvmRoot = 'C:/SDK/llvm-23.1.4'
$hostDependencies = 'C:/SDK/pagos-host-x64-mt'
$env:PATH = "$llvmRoot/bin;$env:PATH"

cmake --preset windows-msvc-release `
    "-DLLVM_DIR=$llvmRoot/lib/cmake/llvm" `
    "-DCMAKE_PREFIX_PATH=$hostDependencies"
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

## Test Selection and Current Limits

The Windows presets set `BUILD_TESTING=ON`, `PAGOS_ENABLE_PROCESS_TESTS=OFF`,
and `PAGOS_ENABLE_LIT_TESTS=OFF`. They build and discover all three GoogleTest
suites. The process and lit suites are explicitly separate because their
Windows adapters are later work; Linux presets retain both suites by default.
`BUILD_TESTING=OFF` omits all testing dependencies and targets.

To request a wider suite, configure with the corresponding option enabled:

```powershell
cmake --preset windows-msvc-release -DPAGOS_ENABLE_LIT_TESTS=ON
```

lit requires Python, lit, FileCheck, Clang, ld.lld, llvm-nm, and llvm-objdump.
Process regressions require Python and Clang. These options never auto-disable
themselves when a tool is missing. Some Windows LLVM development archives omit
FileCheck; supply compatible test tools before enabling lit. Tool availability
alone does not establish that the existing Unix shell, signal, ELF, and shared
library assumptions have been ported.

The preceding investigation found seven Windows unit failures involving
logical path validation/code-page conversion, symbolic-link privileges,
deleting an open test file, and implementation-dependent empty-container
memory accounting. W1 keeps those cases enabled and visible. W2 addresses
them without weakening protocol fixtures or logical resource budgets.

## Compiler Identity and Protocols

The conservative compiler build identity includes CMake module content,
C++ standard mode, frontend variant, and selected CRT, in addition to existing
source/configuration inputs. Changing the host toolchain can invalidate cache
reuse as intended. W1 does not change C1-C4 wire formats, hash domains,
versions, fixed fixtures, or old-format rejection rules; see the
[protocol baseline](cache-protocol-baseline.md).
