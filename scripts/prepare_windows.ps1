param(
    [string]$WorkDirectory = (Join-Path $PSScriptRoot '../build/windows-tools'),
    [string]$LLVMRoot,
    [string]$DependencyPrefix,
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$WorkDirectory = [IO.Path]::GetFullPath($WorkDirectory)
$providedLLVM = [bool]$LLVMRoot
$providedDependencies = [bool]$DependencyPrefix
New-Item -ItemType Directory -Force $WorkDirectory | Out-Null
$lock = Get-Content -Raw (Join-Path $repository 'ci/windows-toolchain.json') | ConvertFrom-Json
. (Join-Path $repository 'ci/windows_environment.ps1')
Enter-PagosWindowsEnvironment

function Invoke-Checked {
    param([string]$Executable, [string[]]$Arguments)
    & $Executable @Arguments | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
}
function Get-VerifiedFile {
    param([string]$Url, [string]$Destination, [string]$Hash)
    if (Test-Path -LiteralPath $Destination) {
        if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Hash) { return }
        throw "Checksum mismatch in existing download: $Destination"
    }
    $partial = $Destination + '.partial'
    Invoke-WebRequest -Uri $Url -OutFile $partial
    if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Hash) {
        throw "Checksum mismatch: $Url"
    }
    Move-Item -LiteralPath $partial -Destination $Destination
}

if (-not $LLVMRoot) {
    $archive = Join-Path $WorkDirectory $lock.llvm_archive
    Get-VerifiedFile $lock.llvm_url $archive $lock.llvm_sha256
    $LLVMRoot = Join-Path $WorkDirectory $lock.llvm_directory
    if (-not (Test-Path -LiteralPath (Join-Path $LLVMRoot 'lib/cmake/llvm/LLVMConfig.cmake'))) {
        Invoke-Checked 'tar.exe' @('-xf', $archive, '-C', $WorkDirectory)
    }
}
$LLVMRoot = [IO.Path]::GetFullPath($LLVMRoot)
Enter-PagosWindowsEnvironment $LLVMRoot
$llvmVersion = (& (Join-Path $LLVMRoot 'bin/llvm-config.exe') --version).Trim()
if ($LASTEXITCODE -ne 0 -or $llvmVersion -ne $lock.llvm_version) { throw 'LLVM SDK version does not match the lock file.' }

if (-not $DependencyPrefix) {
    $vcpkg = Join-Path $WorkDirectory 'vcpkg'
    if (-not (Test-Path -LiteralPath $vcpkg)) {
        Invoke-Checked 'git' @('clone', '--filter=blob:none', '--no-checkout', 'https://github.com/microsoft/vcpkg.git', $vcpkg)
        Invoke-Checked 'git' @('-C', $vcpkg, 'checkout', '--detach', $lock.vcpkg_commit)
    }
    if ((& git -C $vcpkg rev-parse HEAD).Trim() -ne $lock.vcpkg_commit) { throw 'Existing vcpkg checkout does not match the lock file.' }
    if (-not (Test-Path -LiteralPath (Join-Path $vcpkg 'vcpkg.exe'))) {
        Invoke-Checked (Join-Path $vcpkg 'bootstrap-vcpkg.bat') @('-disableMetrics')
    }
    $env:VCPKG_DISABLE_METRICS = '1'
    $installed = Join-Path $WorkDirectory 'installed'
    Invoke-Checked (Join-Path $vcpkg 'vcpkg.exe') @('install', "--vcpkg-root=$vcpkg", '--triplet=x64-windows-static', "--x-manifest-root=$repository/ci/windows-vcpkg", "--x-install-root=$installed")
    $DependencyPrefix = Join-Path $installed 'x64-windows-static'
}
$DependencyPrefix = [IO.Path]::GetFullPath($DependencyPrefix)

$source = Join-Path $WorkDirectory 'FileCheck.cpp'
Get-VerifiedFile $lock.filecheck_url $source $lock.filecheck_sha256
$filecheckBuild = Join-Path $WorkDirectory 'filecheck-build'
Invoke-Checked 'cmake' @('-G', 'Ninja', '-S', "$repository/ci/filecheck", '-B', $filecheckBuild, '-DCMAKE_BUILD_TYPE=Release', "-DLLVM_DIR=$LLVMRoot/lib/cmake/llvm", "-DCMAKE_PREFIX_PATH=$DependencyPrefix", "-DPAGOS_FILECHECK_SOURCE=$source")
Invoke-Checked 'cmake' @('--build', $filecheckBuild, '--parallel', '4')

$venv = Join-Path $WorkDirectory 'python'
$pythonExecutable = Join-Path $venv 'Scripts/python.exe'
if (-not (Test-Path -LiteralPath $pythonExecutable)) { Invoke-Checked $Python @('-m', 'venv', $venv) }
Invoke-Checked $pythonExecutable @('-m', 'pip', 'install', '--only-binary=:all:', '--require-hashes', '-r', "$repository/ci/windows-test-requirements.txt")
$tools = [ordered]@{
    llvm = $LLVMRoot
    dependencies = $DependencyPrefix
    filecheck = (Join-Path $filecheckBuild 'FileCheck.exe')
    python = $pythonExecutable
    lit = (Join-Path $venv 'Scripts/lit.exe')
    llvm_version = $llvmVersion
    sdk_mode = $(if ($providedLLVM) { 'provided-prefix' } else { 'verified-release-archive' })
    dependency_mode = $(if ($providedDependencies) { 'provided-prefix' } else { 'pinned-vcpkg' })
    vcpkg_commit = $(if ($providedDependencies) { $null } else { $lock.vcpkg_commit })
    filecheck_source_sha256 = $lock.filecheck_sha256
    lit_version = (& $pythonExecutable -c 'import importlib.metadata; print(importlib.metadata.version("lit"))').Trim()
    cl_version = (Get-Item (Get-Command cl).Source).VersionInfo.FileVersion
    vs_installation = $env:VSINSTALLDIR
    vc_tools_version = $env:VCToolsVersion
    runner_image_version = $env:ImageVersion
}
$manifest = Join-Path $WorkDirectory 'toolchain.json'
$tools | ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding utf8
Write-Host "Windows toolchain ready: $manifest"
