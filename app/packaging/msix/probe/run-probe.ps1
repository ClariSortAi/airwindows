<#
.SYNOPSIS
Builds the MSIX capability probe, registers it under a test identity, runs every
check with package identity, then unregisters it and removes what it wrote.

.DESCRIPTION
The probe package is made from app/packaging/msix/AppxManifest.template.xml with
the identity GraniteAi.AirWindows.Probe, so it declares the receiver's exact
capabilities and extensions. Output goes to a new folder under app/dist.
Needs Developer Mode and Visual Studio Build Tools; no elevation, no certificate.
The Bluetooth step runs only when no PhonePort/AirWindowsNative/AirWindows process exists,
and only with -Bluetooth. It never launches the receiver.
#>
param(
    [switch] $Bluetooth,
    [string] $OutDir
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$app = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))
Import-Module (Join-Path (Split-Path -Parent $here) 'MsixTools.psm1') -Force
$identity = 'GraniteAi.AirWindows.Probe'
if (-not $OutDir) { $OutDir = Join-Path $app ('dist\msix-probe-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
if (Test-Path -LiteralPath $OutDir) { throw "Output exists; choose a new -OutDir: $OutDir" }
$build = Join-Path $OutDir 'build'
$layout = Join-Path $OutDir 'layout'
New-Item -ItemType Directory -Path $build, $layout -Force | Out-Null
$failures = 0
function Line([string] $Status, [string] $Check, [string] $Detail) {
    if ($Status -eq 'FAIL') { $script:failures++ }
    Write-Host "$Status $Check`: $Detail"
}

# ---- build (the same vcvars64 discovery as app/scripts/build.ps1)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; install Visual Studio Build Tools with the C++ workload.' }
$vcvars = & $vswhere -all -products * -property installationPath |
    ForEach-Object { Join-Path $_ 'VC\Auxiliary\Build\vcvars64.bat' } |
    Where-Object { Test-Path $_ } | Sort-Object | Select-Object -Last 1
if (-not $vcvars) { throw 'vcvars64.bat not found in any Visual Studio installation.' }
cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
}
$source = Join-Path $here 'probe.cpp'
$libs = @('windowsapp.lib', 'ws2_32.lib', 'iphlpapi.lib', 'shell32.lib', 'ole32.lib', 'user32.lib', 'advapi32.lib')
$common = @('/nologo', '/std:c++17', '/EHsc', '/permissive', '/W3', '/O2', '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', "/Fo$build\")
& cl.exe @common $source "/Fe$build\probectl.exe" /link @libs | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'probectl.exe build failed.' }
& cl.exe @common '/DPROBE_GUI' $source "/Fe$build\AirWindowsProbe.exe" /link @libs '/SUBSYSTEM:WINDOWS' '/ENTRY:wmainCRTStartup' | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'AirWindowsProbe.exe build failed.' }

# ---- layout from the real template
Copy-Item -LiteralPath (Join-Path $build 'probectl.exe'), (Join-Path $build 'AirWindowsProbe.exe') -Destination $layout
New-AppxManifest @{
    IDENTITY_NAME = $identity; PUBLISHER = 'CN=AirWindows Probe'; PUBLISHER_DISPLAY_NAME = 'GraniteAi, LLC'
    DISPLAY_NAME = 'AirWindows Probe'; VERSION = '1.0.0.0'
    RECEIVER_EXE = 'AirWindowsProbe.exe'; RECEIVER_PARAMETERS = '--from-manifest'; RECEIVER_ALIAS = 'airwindows-probe.exe'
    IOSCTL_EXE = 'probectl.exe'; IOSCTL_ALIAS = 'probectl.exe'; IOSCTL_DISPLAY_NAME = 'probectl'
} (Join-Path $layout 'AppxManifest.xml')
Copy-MsixAssets $layout
New-ResourcesPri $layout $OutDir

# ---- clean slate: earlier probe registrations and probe folders
Unregister-PackageByName $identity
$local = [Environment]::GetFolderPath('LocalApplicationData')
$probeFolders = @(
    (Join-Path $local 'AirWindows-probe'), (Join-Path $local 'AirWindows-probe-new'),
    (Join-Path ([Environment]::GetFolderPath('MyPictures')) 'AirWindows-probe'),
    (Join-Path ([Environment]::GetFolderPath('MyVideos')) 'AirWindows-probe'))
foreach ($folder in $probeFolders) { if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force } }
New-Item -ItemType Directory -Path $probeFolders[0] | Out-Null
'written unpackaged before registration' | Set-Content -LiteralPath (Join-Path $probeFolders[0] 'existing.txt') -Encoding ascii
$vendorKey = 'HKCU:\Software\GraniteAi'
$vendorKeyExisted = Test-Path -LiteralPath $vendorKey
$probeKeys = @("$vendorKey\AirWindowsProbe-existing", "$vendorKey\AirWindowsProbe-new")
foreach ($key in $probeKeys) { if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force } }
New-Item -Path $probeKeys[0] -Force | Out-Null

function Get-ProbeFirewallRules {
    # Read-only. Rules for executables in this run's layout: the manifest's and any a prompt created.
    @(Get-NetFirewallApplicationFilter -PolicyStore ActiveStore | Where-Object { $_.Program -and $_.Program -like "$layout\*" } |
        Get-NetFirewallRule -PolicyStore ActiveStore)
}
# The receiver-slot exe has no console: it writes to this pipe. Returns the text, or $null on timeout.
function Receive-Report([scriptblock] $Launch, [int] $Seconds = 20) {
    $server = [IO.Pipes.NamedPipeServerStream]::new('AirWindowsProbe.Report', [IO.Pipes.PipeDirection]::In, 1,
        [IO.Pipes.PipeTransmissionMode]::Byte, [IO.Pipes.PipeOptions]::Asynchronous)
    try {
        $wait = $server.WaitForConnectionAsync()
        & $Launch
        if (-not $wait.Wait($Seconds * 1000)) { return $null }
        return ([IO.StreamReader]::new($server)).ReadToEnd().Trim()
    } finally { $server.Dispose() }
}
function Expect-Report([scriptblock] $Launch, [string] $Check, [string] $Expect) {
    $text = Receive-Report $Launch
    if ($null -eq $text) { Line 'FAIL' $Check 'the packaged receiver-slot exe did not report within 20 s'; return }
    $text = $text -replace "`r?`n", ' | '
    if ($text -match 'identity=GraniteAi\.AirWindows\.Probe' -and $text -like "*$Expect*") { Line 'PASS' $Check $text }
    else { Line 'FAIL' $Check "expected '$Expect' with package identity; got: $text" }
}
$before = Get-ProbeFirewallRules
if ($before.Count) { Line 'INFO' 'fw.before' "$($before.Count) matching rule(s) exist before registration" }

Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml')
$installed = Get-PackageByName $identity
try {
    if (-not $installed) { throw "Registration of $identity reported success but the package is not installed." }
    $pfn = $installed[0].PackageFamilyName
    Line 'PASS' 'register' "$($installed[0].PackageFullName) (loose layout, Developer Mode)"

    # ---- firewall rules declared by windows.firewallRules
    $rules = Get-ProbeFirewallRules
    if ($rules.Count) {
        foreach ($rule in $rules) {
            $port = $rule | Get-NetFirewallPortFilter -PolicyStore ActiveStore
            Line 'PASS' 'fw.rule' "'$($rule.DisplayName)' $($rule.Direction) $($port.Protocol) local ports $($port.LocalPort) profile $($rule.Profile) enabled $($rule.Enabled) store $($rule.PolicyStoreSourceType)"
        }
    } else {
        Line 'SKIP' 'fw.rules' 'no rule for AirWindowsProbe.exe after loose registration; the declared rules are not applied by Add-AppxPackage -Register'
    }

    # ---- console alias: resolution, arguments, exit code
    $alias = Get-Command 'probectl.exe' -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($alias) { Line 'PASS' 'alias.resolves' $alias.Source } else { Line 'FAIL' 'alias.resolves' 'probectl.exe not found on PATH' }
    $echo = & probectl.exe echo 'plain' 'with space' '--flag=x' 2>&1
    $code = $LASTEXITCODE
    $echoText = ($echo | Out-String).Trim() -replace "`r?`n", ' | '
    if ($code -eq 7 -and $echoText -match 'identity=GraniteAi\.AirWindows\.Probe' -and $echoText -match 'argv\[3\]=with space' -and $echoText -match 'argv\[4\]=--flag=x') {
        Line 'PASS' 'alias.console' "exit code 7 returned, stdout captured, arguments intact: $echoText"
    } else {
        Line 'FAIL' 'alias.console' "exit $code; $echoText"
    }

    # ---- in-package checks, run by the receiver-slot exe that the firewall rules name
    $receivers = @(Get-Process -Name 'PhonePort', 'AirWindowsNative', 'AirWindows' -ErrorAction SilentlyContinue)
    $runBluetooth = $Bluetooth -and $receivers.Count -eq 0
    if ($Bluetooth -and -not $runBluetooth) { Write-Host 'Bluetooth step not run: a PhonePort receiver is running.' }
    $checkArgs = @('checks'); if ($runBluetooth) { $checkArgs += '--bluetooth' }
    $public = @(Get-NetConnectionProfile | Where-Object NetworkCategory -eq 'Public')
    if ($public) {
        $checkArgs += '--no-wildcard'
        Write-Host "Public network connected ($($public.InterfaceAlias -join ', ')): the UDP 5353 wildcard bind is skipped so Windows shows no firewall prompt."
    }
    $report = Receive-Report { Start-Process -FilePath 'airwindows-probe.exe' -ArgumentList $checkArgs } 120
    if ($null -eq $report) { Line 'FAIL' 'checks' 'the receiver-slot exe did not report within 120 s' }
    else { $report -split "`n" | Where-Object { $_ -match '^(PASS|FAIL|SKIP|INFO) ' } | ForEach-Object { Write-Host $_; if ($_ -like 'FAIL *') { $script:failures++ } } }

    # ---- Store license as seen through the console alias (packaged) and from the unpackaged copy
    & probectl.exe store 2>&1 | ForEach-Object { Write-Host ("$_" -replace '^INFO store\.license:', 'INFO store.license.console-alias:') }
    & (Join-Path $build 'probectl.exe') store 2>&1 | ForEach-Object { Write-Host ("$_" -replace '^INFO store\.license:', 'INFO store.license.unpackaged:') }

    # ---- where the file writes landed, seen from outside the package
    $private = Join-Path $local "Packages\$pfn\LocalCache\Local"
    foreach ($relative in @('AirWindows-probe\existing.txt', 'AirWindows-probe\new.txt', 'AirWindows-probe-new\new.txt')) {
        $real = Join-Path $local $relative
        $redirected = Join-Path $private $relative
        $realHas = (Test-Path -LiteralPath $real) -and ((Get-Content -LiteralPath $real -Raw) -match 'packaged probe')
        $privHas = Test-Path -LiteralPath $redirected
        Line 'INFO' "fs.where.$relative" "real %LOCALAPPDATA% copy has the packaged write: $realHas; package-private copy exists: $privHas"
    }
    foreach ($key in $probeKeys) {
        $value = (Get-ItemProperty -LiteralPath $key -Name 'PackagedWrite' -ErrorAction SilentlyContinue).PackagedWrite
        Line 'INFO' "reg.where.$(Split-Path -Leaf $key)" "value visible in the real HKCU hive from outside the package: $($null -ne $value)"
    }
    foreach ($known in @('MyPictures', 'MyVideos')) {
        $real = Join-Path ([Environment]::GetFolderPath($known)) 'AirWindows-probe\probe.txt'
        Line 'INFO' "fs.where.$known" "real known-folder file exists: $(Test-Path -LiteralPath $real)"
    }

    # ---- pipe, mutex, window and messages across the package boundary
    $serveOut = Join-Path $OutDir 'serve.txt'
    $serve = Start-Process -FilePath 'probectl.exe' -ArgumentList 'serve' -RedirectStandardOutput $serveOut -NoNewWindow -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and -not ((Test-Path $serveOut) -and (Select-String -LiteralPath $serveOut -Pattern '^READY' -Quiet))) {
        Start-Sleep -Milliseconds 200
    }
    $client = & (Join-Path $build 'probectl.exe') client 2>&1
    $client | ForEach-Object { Write-Host $_; if ("$_" -like 'FAIL *') { $script:failures++ } }
    if (-not $serve.WaitForExit(40000)) { $serve.Kill(); Line 'FAIL' 'serve' 'did not exit within 40 s' }
    Get-Content -LiteralPath $serveOut | Where-Object { $_ -notlike 'READY*' } |
        ForEach-Object { Write-Host $_; if ($_ -like 'FAIL *') { $script:failures++ } }

    # ---- windowed alias and Start-menu activation of the receiver slot
    Expect-Report { Start-Process -FilePath 'airwindows-probe.exe' -ArgumentList '--from-alias' } 'alias.windowed' '--from-alias'
    Expect-Report { Start-Process "shell:AppsFolder\$pfn!AirWindows" } 'start-menu-activation' '--from-manifest'

    # ---- did any probe executable trigger a firewall prompt? (the prompt leaves "Query User" rules)
    $prompted = @(Get-ProbeFirewallRules | Where-Object Name -like '*Query User*')
    if ($prompted) { Line 'FAIL' 'fw.no-prompt' "Windows created $($prompted.Count) prompt rule(s): $(($prompted | ForEach-Object DisplayName) -join ', ')" }
    else { Line 'PASS' 'fw.no-prompt' 'no "Query User" firewall rule appeared for the probe executables' }
    $startApp = Get-StartApps | Where-Object { $_.AppID -eq "$pfn!AirWindows" }
    if ($startApp) { Line 'PASS' 'start-menu.entry' "'$($startApp.Name)' $($startApp.AppID)" } else { Line 'FAIL' 'start-menu.entry' 'no Start menu entry' }
    if (Get-StartApps | Where-Object { $_.AppID -eq "$pfn!iosctl" }) { Line 'FAIL' 'start-menu.hidden' 'the console app is listed in Start' }
    else { Line 'PASS' 'start-menu.hidden' 'the console app (AppListEntry none) is not listed in Start' }
} finally {
    Unregister-PackageByName $identity
    if (Get-PackageByName $identity) { Line 'FAIL' 'unregister' "$identity is still registered" } else { Line 'PASS' 'unregister' "$identity removed" }
    $after = Get-ProbeFirewallRules
    if ($after.Count) { Line 'FAIL' 'fw.after-unregister' "$($after.Count) rule(s) remain" } else { Line 'PASS' 'fw.after-unregister' 'no probe firewall rules remain' }
    foreach ($folder in $probeFolders) { if (Test-Path -LiteralPath $folder) { Remove-Item -LiteralPath $folder -Recurse -Force } }
    foreach ($key in $probeKeys) { if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force } }
    if (-not $vendorKeyExisted -and (Test-Path -LiteralPath $vendorKey) -and -not (Get-ChildItem -LiteralPath $vendorKey) -and
        -not (Get-Item -LiteralPath $vendorKey).Property) { Remove-Item -LiteralPath $vendorKey }
    if (Test-Path -LiteralPath (Join-Path $local "Packages\$identity*")) { Line 'INFO' 'cleanup' 'package data folder still present' }
}
Write-Host "Probe output: $OutDir ($failures failure(s))"
exit ([int]($failures -gt 0))
