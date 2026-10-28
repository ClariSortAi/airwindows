param([ValidateSet('Trace','Capture')][string]$Mode = 'Trace')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'artifacts\stock-probe-04'
$exe = Join-Path $build 'stock-radio-probe.exe'
$buildReport = Get-Content -LiteralPath (Join-Path $build 'build-report.json') -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $exe).Hash -ne $buildReport.binary_sha256) { throw 'Diagnostic binary differs from build receipt.' }
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Windows ETW requires an elevated administrator token for this test.' }
$out = Join-Path $root ('artifacts\stock-live-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
if (Test-Path -LiteralPath $out) { throw 'Evidence path already exists.' }
New-Item -ItemType Directory -Path $out | Out-Null
$report = [ordered]@{mode=$Mode; startedUtc=[DateTime]::UtcNow.ToString('o'); radioTestStarted=$false; payloadsSaved=$false; wifiRestored=$null; error=$null}
$profile = $null
$disconnected = $false
try {
    $probe = & $exe --trace-probe 2>&1
    $probe | Set-Content -LiteralPath (Join-Path $out 'trace-probe.jsonl')
    if ($LASTEXITCODE -ne 0) { throw 'Elevated trace probe failed; Wi-Fi unchanged.' }
    if ($Mode -eq 'Capture') {
        $ethernet = Get-NetAdapter -Name 'Ethernet' -ErrorAction Stop
        if ($ethernet.Status -ne 'Up' -or -not (Get-NetRoute -InterfaceIndex $ethernet.ifIndex -DestinationPrefix '0.0.0.0/0' -ErrorAction SilentlyContinue)) {
            throw 'A working Ethernet default route is required before temporarily disconnecting Wi-Fi.'
        }
        $before = (& netsh.exe wlan show interfaces | Out-String)
        $profileMatch = [regex]::Match($before, '(?m)^\s*Profile\s*:\s*(.+?)\s*$')
        if (-not $profileMatch.Success -or $before -notmatch 'State\s*:\s*connected') { throw 'Expected an active Wi-Fi profile to restore.' }
        $profile = $profileMatch.Groups[1].Value.Trim()
        $report['profileToRestore'] = $profile
        $disconnect = & netsh.exe wlan disconnect interface=Wi-Fi
        if ($LASTEXITCODE -ne 0) { throw 'Wi-Fi disconnect failed.' }
        $disconnected = $true
        $disconnect | Set-Content -LiteralPath (Join-Path $out 'disconnect.txt')
        Start-Sleep -Milliseconds 1500
        $report.radioTestStarted = $true
        $process = Start-Process -FilePath $exe -ArgumentList '--capture-idle' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $out 'capture.jsonl') -RedirectStandardError (Join-Path $out 'capture-stderr.txt')
        $null = $process.Handle
        if (-not $process.WaitForExit(20000)) {
            Stop-Process -Id $process.Id -Force
            throw 'Bounded capture timed out; attempting sniffer stop and Wi-Fi restoration.'
        }
        $report['captureExitCode'] = $process.ExitCode
    }
} catch {
    $report.error = $_.Exception.Message
} finally {
    if ($report.radioTestStarted) {
        & $exe --stop 2>&1 | Set-Content -LiteralPath (Join-Path $out 'stop.jsonl')
        $report['stopExitCode'] = $LASTEXITCODE
    }
    if ($disconnected -and $profile) {
        & netsh.exe wlan connect "name=$profile" interface=Wi-Fi | Set-Content -LiteralPath (Join-Path $out 'reconnect.txt')
        $report.wifiRestored = $false
        for ($attempt = 0; $attempt -lt 15; $attempt++) {
            Start-Sleep -Milliseconds 1000
            $state = (& netsh.exe wlan show interfaces | Out-String)
            $currentProfile = [regex]::Match($state, '(?m)^\s*Profile\s*:\s*(.+?)\s*$')
            if ($state -match 'State\s*:\s*connected' -and $currentProfile.Success -and $currentProfile.Groups[1].Value.Trim() -eq $profile) {
                $report.wifiRestored = $true
                break
            }
        }
    }
    $report['finishedUtc'] = [DateTime]::UtcNow.ToString('o')
    $report | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'result.json')
}
Write-Output $out
if ($report.error -or ($disconnected -and -not $report.wifiRestored)) { exit 1 }
