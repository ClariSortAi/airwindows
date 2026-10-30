<# Run native policy checks without a phone, Bluetooth publication or dependencies. #>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with the C++ workload.' }
$vcvars = & $vswhere -all -products * -property installationPath |
    ForEach-Object { Join-Path $_ 'VC\Auxiliary\Build\vcvars64.bat' } |
    Where-Object { Test-Path -LiteralPath $_ } | Sort-Object | Select-Object -Last 1
if (-not $vcvars) { throw 'MSVC x64 environment not found.' }
cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
}
$output = Join-Path $root 'app\build\policy'
New-Item -ItemType Directory -Path $output -Force | Out-Null
Push-Location $output
try {
    & cl.exe /nologo /std:c++17 /EHsc /W4 /utf-8 "/I$root\app\src" "/I$root\native-ble-prototype" "$root\app\tests\control_policy_check.cpp" /Fe:ControlPolicyCheck.exe
    if ($LASTEXITCODE -ne 0) { throw 'Control policy build failed.' }
    & .\ControlPolicyCheck.exe
    if ($LASTEXITCODE -ne 0) { throw 'Control policy checks failed.' }
    & cl.exe /nologo /std:c++17 /EHsc /W4 "$root\native-ble-prototype\test_discovery_window.cpp" /Fe:DiscoveryPolicyCheck.exe
    if ($LASTEXITCODE -ne 0) { throw 'Discovery policy build failed.' }
    & .\DiscoveryPolicyCheck.exe
    if ($LASTEXITCODE -ne 0) { throw 'Discovery policy checks failed.' }
} finally {
    Pop-Location
}
