<#
.SYNOPSIS
Configures and builds AirWindows with CMake + Ninja and the project-local vcpkg.

.DESCRIPTION
Loads the MSVC x64 environment from vcvars64.bat, then drives CMake with the
Ninja generator. The Visual Studio generators are avoided on purpose: they
reject a Build Tools instance that vswhere reports as incomplete, while the
compiler itself works fine from the vcvars environment.

The configure step also has vcpkg build the libraries listed in vcpkg.json
(OpenSSL, libplist, FFmpeg) into .deps\vcpkg_installed. The first run compiles
them from source and takes a while; later runs find them already built.

.PARAMETER Config
Release (default), RelWithDebInfo or Debug.

.PARAMETER Target
Optional single CMake target to build.
#>
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string] $Config = 'Release',
    [string] $Target
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vcpkg = Join-Path $root '.deps\vcpkg'
$toolchain = Join-Path $vcpkg 'scripts\buildsystems\vcpkg.cmake'
if (-not (Test-Path $toolchain)) { throw "vcpkg not found at $vcpkg. Run scripts\bootstrap.ps1 first." }

function Find-Tool([string] $name, [string] $vcpkgPattern) {
    $onPath = (Get-Command $name -ErrorAction SilentlyContinue).Source
    if ($onPath) { return $onPath }
    $found = Get-ChildItem "$vcpkg\downloads\tools\$vcpkgPattern" -Recurse -Filter "$name.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $found) { throw "$name.exe not found on PATH or under $vcpkg\downloads\tools\$vcpkgPattern" }
    return $found
}

$cmake = Find-Tool 'cmake' 'cmake-*'
$ninja = Find-Tool 'ninja' 'ninja-*'

# Import the MSVC x64 environment into this process.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; install Visual Studio Build Tools with the C++ workload.' }
$vcvars = & $vswhere -all -products * -property installationPath |
    ForEach-Object { Join-Path $_ 'VC\Auxiliary\Build\vcvars64.bat' } |
    Where-Object { Test-Path $_ } | Sort-Object | Select-Object -Last 1
if (-not $vcvars) { throw 'vcvars64.bat not found in any Visual Studio installation.' }
cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "cl.exe is not available after loading $vcvars" }

$build = Join-Path $root "build\$Config"
& $cmake -S $root -B $build -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_BUILD_TYPE=$Config" `
    '-DCMAKE_C_COMPILER=cl' '-DCMAKE_CXX_COMPILER=cl' `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
    '-DVCPKG_TARGET_TRIPLET=x64-windows-static' `
    '-DVCPKG_MANIFEST_MODE=ON' `
    "-DVCPKG_INSTALLED_DIR=$(Join-Path $root '.deps\vcpkg_installed')"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

$buildArgs = @('--build', $build)
if ($Target) { $buildArgs += @('--target', $Target) }
# -k 0 keeps going after errors so one run reports every failing file.
$buildArgs += @('--', '-k', '0')
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
