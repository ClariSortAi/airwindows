param([Parameter(Mandatory)] [ValidateRange(1,2147483647)] [int] $ReceiverPid, [string] $IoscTlPath)
$ErrorActionPreference = 'Stop'
if (-not $IoscTlPath) {
    $candidates = @((Join-Path $PSScriptRoot '..\..\..\iosctl.exe'), (Join-Path $PSScriptRoot '..\..\build\Release\bin\iosctl.exe'))
    $IoscTlPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $IoscTlPath) { throw 'iosctl.exe not found. Use the Windows package or build the helper.' }
& $IoscTlPath --pid $ReceiverPid 'preflight'
if ($LASTEXITCODE -ne 0) { throw 'Phone preflight failed. Inspect the helper result before sending input.' }
