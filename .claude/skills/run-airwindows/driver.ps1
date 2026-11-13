<#
Agent driver for the AirWindows receiver. Runs the real AirWindowsNative.exe in
self-test mode (a looping H.264 clip instead of an iPhone), then observes and
pokes the running window from outside without taking focus.

Usage (PowerShell 7, from the repository root):
  ./.claude/skills/run-airwindows/driver.ps1 <command> [args]

Commands:
  clip [w h]          Make the looping test clip (default 1170x2532, phone portrait)
  launch [seconds] [bt]  Start the receiver on the clip (default safety exit 600 s).
                      Saved phones are hidden so no iPhone binds; 'bt' keeps them.
  status              Process, window title and frame-pipe metadata (JSON)
  frame <out.png>     Save the decoded picture at native size (frame pipe)
  ss <out.png>        Save the whole receiver window, chrome included
  cmd <name|id>       Post a WM_COMMAND to the window (see $Commands below)
  log [regex]         Log lines written since this launch (optionally filtered)
  stop                Close the receiver, restore settings.ini, print the summary

State and captures live in .local/run-airwindows/ (git-ignored).
#>
param([Parameter(Position = 0)][string]$Command = 'help',
      [Parameter(Position = 1, ValueFromRemainingArguments)][string[]]$Rest)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Exe = Join-Path $Root 'app\build\Release\bin\AirWindowsNative.exe'
$Work = Join-Path $Root '.local\run-airwindows'
$StateFile = Join-Path $Work 'state.json'
$Clip = Join-Path $Work 'clip.h264'
$AppData = Join-Path $env:LOCALAPPDATA 'AirWindows'
$Log = Join-Path $AppData 'debug.txt'
$Settings = Join-Path $AppData 'settings.ini'
New-Item -ItemType Directory -Force $Work | Out-Null

# Safe commands only. Phone/Bluetooth ids (41001-41005, 41100+) are left out on
# purpose: they pair, bind or disconnect a real phone. Settings-changing ids
# (decoder, fps, vsync, quality) persist; stop restores settings.ini anyway.
$Commands = [ordered]@{
    screenshot = 40060; record = 40061; fullscreen = 40040; fit = 40041
    ontop = 40042; details = 40043; guide = 40052; 'guide-open' = 41011
    'guide-close' = 41012; 'dump-menu' = 41200
}
# Refused: Bluetooth (above) and anything that restarts the AirPlay service
# (rename, restart, decoder, fps, quality, H.265): that path can start the real
# network listener from a self-test receiver.
$Refused = @(40001, 40002, 40010, 40011, 40020, 40021, 40022, 40031) + (41001..41005) + (41100..41131) + (40100..40199)

if (-not ('AwDriver.Native' -as [type])) {
    Add-Type -Namespace AwDriver -Name Native -MemberDefinition @'
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string title);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
public struct RECT { public int Left, Top, Right, Bottom; }
public static IntPtr WindowOf(uint pid) {
    IntPtr h = IntPtr.Zero;
    while ((h = FindWindowExW(IntPtr.Zero, h, "AirWindowsMainWindow", null)) != IntPtr.Zero) {
        uint owner; GetWindowThreadProcessId(h, out owner); if (owner == pid) return h;
    }
    return IntPtr.Zero;
}
'@
}
# Per-monitor DPI awareness, or GetWindowRect/PrintWindow sizes are scaled.
[void][AwDriver.Native]::SetProcessDpiAwarenessContext([IntPtr]-4)

function Get-State {
    if (-not (Test-Path $StateFile)) { throw "No receiver launched by this driver (missing $StateFile). Run: launch" }
    Get-Content $StateFile -Raw | ConvertFrom-Json
}
function Get-LiveWindow($state) {
    $p = Get-Process -Id $state.pid -ErrorAction SilentlyContinue
    if (-not $p) { throw "Receiver PID $($state.pid) is not running (exited or --exit-after elapsed). Run: log, then launch" }
    $h = [AwDriver.Native]::WindowOf([uint32]$state.pid)
    if ($h -eq [IntPtr]::Zero) { throw "PID $($state.pid) has no AirWindowsMainWindow" }
    $h
}
function Get-NewLog($state) {
    if (-not (Test-Path $Log)) { return @() }
    $fs = [IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
    try {
        # The log rotates at 4 MB; if it shrank, read it all.
        $start = if ($fs.Length -ge $state.logOffset) { [int64]$state.logOffset } else { 0 }
        $fs.Position = $start
        $text = (New-Object IO.StreamReader($fs)).ReadToEnd()
    } finally { $fs.Dispose() }
    $text -split "`r?`n" | Where-Object { $_ }
}
function Invoke-FrameReader([int]$receiverPid, [string]$out) {
    # The bundled readers (iosctl, observe-phone.py) reject source=selftest by
    # design; the library behind observe-phone.py takes allow_selftest.
    $py = @'
import json, sys
sys.path.insert(0, sys.argv[1])
from pathlib import Path
from airwindows_frames import AirPlayReader
r = AirPlayReader(int(sys.argv[2]), allow_selftest=True)
if sys.argv[3] == "-":
    _, meta = r.grab()
else:
    meta = r.save(Path(sys.argv[3]))
print(json.dumps(meta))
'@
    $result = python -c $py (Join-Path $Root 'app\scripts') $receiverPid $out 2>&1
    if ($LASTEXITCODE -ne 0) { throw "frame reader failed: $result" }
    $result | Select-Object -Last 1
}

switch ($Command) {
    'clip' {
        $w = if ($Rest.Count -ge 2) { $Rest[0] } else { '1170' }
        $h = if ($Rest.Count -ge 2) { $Rest[1] } else { '2532' }
        # Annex-B H.264 is what --selftest-video expects; mp4 does not work.
        ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=${w}x${h}:rate=30" -t 4 `
            -c:v libx264 -pix_fmt yuv420p -bsf:v h264_mp4toannexb -f h264 $Clip
        if ($LASTEXITCODE -ne 0) { throw 'ffmpeg failed' }
        "clip: $Clip ($((Get-Item $Clip).Length) bytes, ${w}x${h})"
    }
    'launch' {
        if (-not (Test-Path $Exe)) { throw "Missing $Exe. Build: ./app/scripts/build.ps1 -Target AirWindowsNative" }
        if (-not (Test-Path $Clip)) { & $PSCommandPath clip }
        if (Test-Path $StateFile) {
            $old = Get-Content $StateFile -Raw | ConvertFrom-Json
            if (Get-Process -Id $old.pid -ErrorAction SilentlyContinue) { throw "Driver receiver PID $($old.pid) still running. Run: stop" }
        }
        $seconds = if ($Rest.Count) { [int]$Rest[0] } else { 600 }
        $capture = Join-Path $Work ('capture-' + (Get-Date -Format 'HHmmss'))
        New-Item -ItemType Directory -Force $capture | Out-Null
        # The self-test receiver shares settings.ini with the user's real one and
        # saves window placement on close. Back it up; stop restores it.
        $backup = $null
        if (Test-Path $Settings) { $backup = Join-Path $Work 'settings.ini.bak'; Copy-Item $Settings $backup -Force }
        # Self-test runs still start Bluetooth control, and with saved phones they
        # publish the HID service and the user's iPhone binds to this receiver.
        # Hide the saved phones (NativeControl*) while it reads settings, unless
        # the second argument is 'bt'. settings.ini is UTF-16LE.
        $keepBt = $Rest.Count -ge 2 -and $Rest[1] -eq 'bt'
        if ($backup -and -not $keepBt) {
            Get-Content $backup -Encoding Unicode | Where-Object { $_ -notmatch '^NativeControl' } |
                Set-Content $Settings -Encoding Unicode
        }
        $offset = if (Test-Path $Log) { (Get-Item $Log).Length } else { 0 }
        $argList = @('--selftest-video', "`"$Clip`"", '--frames', '--selftest-menu', '--capture-dir', "`"$capture`"", '--exit-after', $seconds)
        $p = Start-Process -FilePath $Exe -ArgumentList $argList -PassThru
        $state = [ordered]@{ pid = $p.Id; logOffset = $offset; capture = $capture; settingsBackup = $backup; started = (Get-Date).ToString('o') }
        $state | ConvertTo-Json | Set-Content $StateFile
        # Ready = window exists and the pipe serves a fresh selftest frame.
        $deadline = (Get-Date).AddSeconds(20); $meta = $null; $restored = $false
        while ((Get-Date) -lt $deadline -and -not $meta) {
            Start-Sleep -Milliseconds 500
            if ($p.HasExited) { if ($backup) { Copy-Item $backup $Settings -Force }; throw "Receiver exited with $($p.ExitCode). Run: log" }
            if ([AwDriver.Native]::WindowOf([uint32]$p.Id) -eq [IntPtr]::Zero) { continue }
            # Settings are read before the window exists: put the user's file back
            # now so a real receiver started meanwhile still sees its phones.
            if ($backup -and -not $restored) { Copy-Item $backup $Settings -Force; $restored = $true }
            try { $meta = Invoke-FrameReader $p.Id '-' | ConvertFrom-Json } catch { $last = $_.Exception.Message }
        }
        if (-not $meta) { throw "No fresh frame within 20 s ($last). Run: log" }
        $bt = if ($keepBt) { 'saved phones kept (bt)' } else { 'saved phones hidden' }
        "launched pid=$($p.Id) picture=$($meta.width)x$($meta.height) capture=$capture exit-after=${seconds}s bluetooth=$bt"
    }
    'status' {
        $state = Get-State; $h = Get-LiveWindow $state
        $sb = New-Object Text.StringBuilder 256; [void][AwDriver.Native]::GetWindowTextW($h, $sb, 256)
        $meta = Invoke-FrameReader $state.pid '-' | ConvertFrom-Json
        [ordered]@{ pid = $state.pid; title = $sb.ToString(); width = $meta.width; height = $meta.height
            sequence = $meta.sequence; frame_age_ms = $meta.frame_age_ms; mirror_active = $meta.mirror_active
            control_ready = $meta.control_ready; source = $meta.source } | ConvertTo-Json
    }
    'frame' {
        if (-not $Rest.Count) { throw 'usage: frame <out.png>' }
        $state = Get-State; [void](Get-LiveWindow $state)
        $out = [IO.Path]::GetFullPath($Rest[0], (Get-Location).Path)
        $meta = Invoke-FrameReader $state.pid $out | ConvertFrom-Json
        "frame: $out ($($meta.width)x$($meta.height), seq $($meta.sequence), age $($meta.age_ms_at_read) ms)"
    }
    'ss' {
        if (-not $Rest.Count) { throw 'usage: ss <out.png>' }
        $state = Get-State; $h = Get-LiveWindow $state
        $out = [IO.Path]::GetFullPath($Rest[0], (Get-Location).Path)
        Add-Type -AssemblyName System.Drawing
        $r = New-Object AwDriver.Native+RECT; [void][AwDriver.Native]::GetWindowRect($h, [ref]$r)
        $bmp = New-Object Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
        $g = [Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
        # 2 = PW_RENDERFULLCONTENT: needed for the Direct3D picture; works while
        # the window is covered by others, but not while it is minimized.
        $ok = [AwDriver.Native]::PrintWindow($h, $hdc, 2)
        $g.ReleaseHdc($hdc); $g.Dispose()
        if (-not $ok) { $bmp.Dispose(); throw 'PrintWindow failed (window minimized?)' }
        New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
        $bmp.Save($out, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
        "ss: $out ($($r.Right - $r.Left)x$($r.Bottom - $r.Top))"
    }
    'cmd' {
        if (-not $Rest.Count) { "commands: $($Commands.Keys -join ', ') (or a numeric id)"; break }
        $state = Get-State; $h = Get-LiveWindow $state
        $id = if ($Commands.Contains($Rest[0])) { $Commands[$Rest[0]] } elseif ($Rest[0] -match '^\d+$') { [int]$Rest[0] } else { throw "unknown command '$($Rest[0])'; known: $($Commands.Keys -join ', ')" }
        if ($Refused -contains $id) { throw "id $id drives Bluetooth or restarts the AirPlay service; refused" }
        # WM_COMMAND = 0x0111. Posted, so it returns before the command runs.
        if (-not [AwDriver.Native]::PostMessageW($h, 0x0111, [IntPtr]$id, [IntPtr]::Zero)) { throw 'PostMessage failed' }
        "posted WM_COMMAND $id"
    }
    'log' {
        $state = Get-State
        $lines = Get-NewLog $state
        if ($Rest.Count) { $lines = $lines | Where-Object { $_ -match $Rest[0] } }
        $lines
    }
    'stop' {
        $state = Get-State
        $p = Get-Process -Id $state.pid -ErrorAction SilentlyContinue
        if ($p) {
            $h = [AwDriver.Native]::WindowOf([uint32]$state.pid)
            if ($h -ne [IntPtr]::Zero) { [void][AwDriver.Native]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) }  # WM_CLOSE
            if (-not $p.WaitForExit(15000)) { Write-Warning "PID $($state.pid) did not close in 15 s; killing"; $p.Kill() }
        }
        if ($state.settingsBackup -and (Test-Path $state.settingsBackup)) { Copy-Item $state.settingsBackup $Settings -Force; 'settings.ini restored' }
        Get-NewLog $state | Where-Object { $_ -match '\[SELFTEST\] video summary|\[CAPTURE\]|\[RECORD\] saved|ERROR|crash' }
        Remove-Item $StateFile
        "stopped pid=$($state.pid); captures in $($state.capture)"
    }
    default { Get-Content $PSCommandPath -TotalCount 21 | Select-Object -Skip 1 }
}
