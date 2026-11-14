<#
Agent driver for the PhonePort receiver. Runs the real PhonePort.exe in
self-test mode (a looping H.264 clip instead of an iPhone), then observes and
pokes the running window from outside without taking focus.

Usage (PowerShell 7, from the repository root):
  ./.claude/skills/run-airwindows/driver.ps1 <command> [args]

Commands:
  clip [w h]          Make the looping test clip (default 1170x2532, phone portrait)
  launch [seconds] [bt]  Start the receiver on the clip (default safety exit 600 s).
                      Saved phones are hidden so no iPhone binds; 'bt' keeps them.
                      -Exe <path> runs another build (or $env:AIRWINDOWS_DRIVER_EXE);
                      default app\build\Release\bin\PhonePort.exe, or the
                      AirWindowsNative.exe a build from before the rename wrote;
                      -ReceiverArgs a,b adds arguments; -NoFrame waits for the window
                      only (automatic with --selftest-license expired).
                      -Clip <file> plays another Annex-B clip (.h265/.hevc = H.265);
                      -NoClip runs without --selftest-video (idle screen; pass a
                      --selftest-* flag in -ReceiverArgs so it stays a self-test).
                      -Packaged starts the registered MSIX package through its
                      phoneport.exe alias (-IdentityName, default GraniteAi.PhonePort.Dev)
                      and works on the package's data folder.
                      -KnownFolders leaves out --capture-dir: captures go to the real
                      Pictures\PhonePort and Videos\PhonePort (delete them after).
  status              Process, window title and frame-pipe metadata (JSON)
  frame <out.png>     Save the decoded picture at native size (frame pipe)
  ss <out.png>        Save the whole receiver window, chrome included
  cmd <name|id>       Post a WM_COMMAND to the window (see $Commands below)
  move <x> <y> [w h]  Move (and size) the window without activating it (physical pixels)
  monitors            List displays: bounds, work area, DPI
  log [regex]         Log lines written since this launch (optionally filtered)
  datadir             The data folder (settings.ini, debug.txt, crashes) of this launch,
                      or with -Packaged the package's, else %LOCALAPPDATA%\AirWindows
  stop                Close the receiver, restore settings.ini, print the summary

State and captures live in .local/run-airwindows/ (git-ignored).
#>
param([Parameter(Position = 0)][string]$Command = 'help',
      [string]$Exe,
      [string[]]$ReceiverArgs = @(),
      [switch]$NoFrame,
      [string]$Clip,
      [switch]$NoClip,
      [switch]$Packaged,
      [switch]$KnownFolders,
      [string]$IdentityName = 'GraniteAi.PhonePort.Dev',
      [Parameter(Position = 1, ValueFromRemainingArguments)][string[]]$Rest)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $Exe) { $Exe = $env:AIRWINDOWS_DRIVER_EXE }
# Process names of a receiver: PhonePort.exe now, and the same program's names from
# before the rename (AirWindowsNative.exe, AirWindows.exe).
$ReceiverNames = @('PhonePort', 'AirWindowsNative', 'AirWindows')
if ($Exe) {
    $Exe = [IO.Path]::GetFullPath($Exe, (Get-Location).Path)
} else {
    $Exe = Join-Path $Root 'app\build\Release\bin\PhonePort.exe'
    $older = Join-Path $Root 'app\build\Release\bin\AirWindowsNative.exe'
    if (-not (Test-Path $Exe) -and (Test-Path $older)) { $Exe = $older }
}
$Work = Join-Path $Root '.local\run-airwindows'
$StateFile = Join-Path $Work 'state.json'
$DefaultClip = Join-Path $Work 'clip.h264'
$Clip = if ($Clip) { [IO.Path]::GetFullPath($Clip, (Get-Location).Path) } else { $DefaultClip }
# Unpackaged runs keep settings and the log in %LOCALAPPDATA%\AirWindows. A packaged run
# uses the package's own data folder (ApplicationData LocalFolder, measured as
# %LOCALAPPDATA%\Packages\<PackageFamilyName>\LocalState); its first run copies
# settings.ini from %LOCALAPPDATA%\AirWindows.
function Get-PackageDataDir([string]$name) {
    $package = @(Get-AppxPackage -Name $name)
    if ($package.Count -ne 1) { throw "Package $name is not registered (found $($package.Count)). Register it: ./app/scripts/package-msix.ps1 ... -Register" }
    Join-Path $env:LOCALAPPDATA "Packages\$($package[0].PackageFamilyName)\LocalState"
}
$ClassicData = Join-Path $env:LOCALAPPDATA 'AirWindows'
$AppData = if ($Packaged -and $Command -in @('launch', 'datadir')) { Get-PackageDataDir $IdentityName } else { $ClassicData }
New-Item -ItemType Directory -Force $Work | Out-Null

# Safe commands only. Phone/Bluetooth ids (41001-41005, 41100+) are left out on
# purpose: they pair, bind or disconnect a real phone. Settings-changing ids
# (decoder, fps, vsync, quality) persist; stop restores settings.ini anyway.
$Commands = [ordered]@{
    screenshot = 40060; record = 40061; fullscreen = 40040; fit = 40041
    ontop = 40042; details = 40043; guide = 40052; 'guide-open' = 41011
    'guide-close' = 41012; 'dump-menu' = 41200
    # Help > Buy PhonePort (Store trial). With --selftest-license it simulates a
    # successful purchase; a self-test never reaches the Store.
    buy = 40053
    # With --selftest-license only: the simulated trial runs out now (then 'buy').
    expire = 41201
    # With --selftest-menu (always passed here) these write the folder or Settings page to
    # the log instead of opening Explorer or Settings.
    'open-log' = 40003; 'open-screenshots' = 40062; 'open-recordings' = 40063; network = 41013
    # Help > PhonePort support; with --selftest-menu it logs the support URL instead of opening it.
    support = 40054
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
if (-not ('AwDriver.Display' -as [type])) {
    Add-Type -Namespace AwDriver -Name Display -MemberDefinition @'
[DllImport("user32.dll")] static extern bool EnumDisplayMonitors(IntPtr dc, IntPtr clip, MonitorProc f, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool GetMonitorInfoW(IntPtr m, ref MONITORINFO i);
[DllImport("shcore.dll")] static extern int GetDpiForMonitor(IntPtr m, int type, out uint x, out uint y);
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
delegate bool MonitorProc(IntPtr m, IntPtr dc, IntPtr r, IntPtr l);
struct RECTI { public int Left, Top, Right, Bottom; }
struct MONITORINFO { public int cbSize; public RECTI Monitor, Work; public uint Flags; }
public static string Monitors() {
    var lines = new System.Text.StringBuilder();
    EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (m, dc, r, l) => {
        var info = new MONITORINFO { cbSize = Marshal.SizeOf(typeof(MONITORINFO)) };
        GetMonitorInfoW(m, ref info);
        uint x, y; GetDpiForMonitor(m, 0, out x, out y);
        lines.AppendFormat("monitor {0},{1} {2}x{3} work {4},{5} {6}x{7} dpi {8} ({9}%){10}\n",
            info.Monitor.Left, info.Monitor.Top, info.Monitor.Right - info.Monitor.Left, info.Monitor.Bottom - info.Monitor.Top,
            info.Work.Left, info.Work.Top, info.Work.Right - info.Work.Left, info.Work.Bottom - info.Work.Top,
            x, x * 100 / 96, (info.Flags & 1) != 0 ? " primary" : "");
        return true;
    }, IntPtr.Zero);
    return lines.ToString();
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
    $Log = if ($state.log) { $state.log } else { Join-Path $ClassicData 'debug.txt' }
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
        if (-not $Packaged -and -not (Test-Path $Exe)) { throw "Missing $Exe. Build: ./app/scripts/build.ps1 -Target AirWindowsNative (it writes PhonePort.exe)" }
        if (-not $NoClip -and -not (Test-Path $Clip)) {
            if ($Clip -ne $DefaultClip) { throw "Missing clip $Clip" }
            & $PSCommandPath clip
        }
        if ($NoClip -and -not ($ReceiverArgs -match '^--selftest-')) {
            throw '-NoClip needs a --selftest-* flag in -ReceiverArgs; without one the receiver would start the real AirPlay listener.'
        }
        if (Test-Path $StateFile) {
            $old = Get-Content $StateFile -Raw | ConvertFrom-Json
            if (Get-Process -Id $old.pid -ErrorAction SilentlyContinue) { throw "Driver receiver PID $($old.pid) still running. Run: stop" }
        }
        $seconds = if ($Rest.Count) { [int]$Rest[0] } else { 600 }
        $Log = Join-Path $AppData 'debug.txt'
        $Settings = Join-Path $AppData 'settings.ini'
        $keepBt = $Rest.Count -ge 2 -and $Rest[1] -eq 'bt'
        # The first packaged run copies %LOCALAPPDATA%\AirWindows\settings.ini, saved phones
        # included. Without 'bt', seed the package's copy without the saved phones first (the
        # receiver then has nothing to carry over); with 'bt', let the receiver copy it.
        $classicSettings = Join-Path $ClassicData 'settings.ini'
        if ($Packaged -and -not (Test-Path $Settings) -and -not $keepBt -and (Test-Path $classicSettings)) {
            Get-Content $classicSettings -Encoding Unicode | Where-Object { $_ -notmatch '^NativeControl' } |
                Set-Content $Settings -Encoding Unicode
            "first packaged run: seeded $Settings without the saved phones (the carry-over itself runs only with 'bt')"
        }
        $capture = Join-Path $Work ('capture-' + (Get-Date -Format 'HHmmss'))
        if (-not $KnownFolders) { New-Item -ItemType Directory -Force $capture | Out-Null }
        # The self-test receiver shares settings.ini with the user's real one and
        # saves window placement on close. Back it up; stop restores it.
        $backup = $null
        if (Test-Path $Settings) { $backup = Join-Path $Work 'settings.ini.bak'; Copy-Item $Settings $backup -Force }
        # Self-test runs still start Bluetooth control, and with saved phones they
        # publish the HID service and the user's iPhone binds to this receiver.
        # Hide the saved phones (NativeControl*) while it reads settings, unless
        # the second argument is 'bt'. settings.ini is UTF-16LE.
        if ($backup -and -not $keepBt) {
            Get-Content $backup -Encoding Unicode | Where-Object { $_ -notmatch '^NativeControl' } |
                Set-Content $Settings -Encoding Unicode
        }
        $offset = if (Test-Path $Log) { (Get-Item $Log).Length } else { 0 }
        $argList = @()
        if (-not $NoClip) { $argList += @('--selftest-video', "`"$Clip`"") }
        $argList += @('--frames', '--selftest-menu', '--exit-after', $seconds)
        # -KnownFolders: screenshots and recordings go to the user's real Pictures\PhonePort and
        # Videos\PhonePort. Delete the files the log names afterwards.
        if (-not $KnownFolders) { $argList += @('--capture-dir', "`"$capture`"") } else { $capture = 'Pictures\PhonePort and Videos\PhonePort (known folders)' }
        $argList += $ReceiverArgs | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
        # A locked receiver (trial over) shows the unlock panel and plays no clip.
        $joined = $ReceiverArgs -join ' '
        $noFrame = $NoFrame -or $NoClip -or $joined -match '--selftest-license\s+expired\b'
        $launchedAt = Get-Date
        if ($Packaged) {
            # The execution alias gives the process package identity; running the exe by
            # its path would not. uap10:Parameters (--frames) applies to Start menu launches only.
            $p = Start-Process -FilePath 'phoneport.exe' -ArgumentList $argList -PassThru
            Start-Sleep -Milliseconds 300
            if ($p.HasExited -or $p.ProcessName -notin $ReceiverNames) {
                $p = Get-Process -Name $ReceiverNames -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $launchedAt.AddSeconds(-1) } |
                    Sort-Object StartTime | Select-Object -Last 1
                if (-not $p) { if ($backup) { Copy-Item $backup $Settings -Force }; throw 'phoneport.exe alias did not leave a receiver process. Run: log' }
            }
            $Exe = $p.Path
        } else {
            $p = Start-Process -FilePath $Exe -ArgumentList $argList -PassThru
        }
        $state = [ordered]@{ pid = $p.Id; logOffset = $offset; capture = $capture; settingsBackup = $backup; started = $launchedAt.ToString('o'); exe = $Exe
            log = $Log; settings = $Settings; packaged = [bool]$Packaged }
        $state | ConvertTo-Json | Set-Content $StateFile
        # Ready = window exists and the pipe serves a fresh selftest frame (or, with
        # no frame expected, the window exists).
        $deadline = (Get-Date).AddSeconds(20); $meta = $null; $restored = $false; $ready = $false
        while ((Get-Date) -lt $deadline -and -not $ready) {
            Start-Sleep -Milliseconds 500
            if ($p.HasExited) { if ($backup) { Copy-Item $backup $Settings -Force }; throw "Receiver exited with $($p.ExitCode). Run: log" }
            if ([AwDriver.Native]::WindowOf([uint32]$p.Id) -eq [IntPtr]::Zero) { continue }
            # Settings are read before the window exists: put the user's file back
            # now so a real receiver started meanwhile still sees its phones.
            if ($backup -and -not $restored) { Copy-Item $backup $Settings -Force; $restored = $true }
            if ($noFrame) { $ready = $true; break }
            try { $meta = Invoke-FrameReader $p.Id '-' | ConvertFrom-Json; $ready = $true } catch { $last = $_.Exception.Message }
        }
        if (-not $ready) { throw "No fresh frame within 20 s ($last). Run: log" }
        $bt = if ($keepBt) { 'saved phones kept (bt)' } else { 'saved phones hidden' }
        $picture = if ($meta) { "$($meta.width)x$($meta.height)" } else { 'none (no frame expected)' }
        "launched pid=$($p.Id) exe=$Exe picture=$picture capture=$capture exit-after=${seconds}s bluetooth=$bt data=$AppData"
    }
    'datadir' {
        if (-not $Packaged -and (Test-Path $StateFile)) { $state = Get-Content $StateFile -Raw | ConvertFrom-Json; if ($state.log) { Split-Path $state.log; break } }
        $AppData
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
    'move' {
        if ($Rest.Count -lt 2) { throw 'usage: move <x> <y> [width height]' }
        $state = Get-State; $h = Get-LiveWindow $state
        # 0x0001 SWP_NOSIZE | 0x0004 SWP_NOZORDER | 0x0010 SWP_NOACTIVATE: no focus change.
        $sized = $Rest.Count -ge 4
        $w = if ($sized) { [int]$Rest[2] } else { 0 }; $hh = if ($sized) { [int]$Rest[3] } else { 0 }
        $flags = if ($sized) { 0x0014 } else { 0x0015 }
        if (-not [AwDriver.Display]::SetWindowPos($h, [IntPtr]::Zero, [int]$Rest[0], [int]$Rest[1], $w, $hh, $flags)) { throw 'SetWindowPos failed' }
        $r = New-Object AwDriver.Native+RECT; [void][AwDriver.Native]::GetWindowRect($h, [ref]$r)
        "moved to $($r.Left),$($r.Top) ($($r.Right - $r.Left)x$($r.Bottom - $r.Top))"
    }
    'monitors' { [AwDriver.Display]::Monitors().TrimEnd() }
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
        $Settings = if ($state.settings) { $state.settings } else { Join-Path $ClassicData 'settings.ini' }
        if ($state.settingsBackup -and (Test-Path $state.settingsBackup)) { Copy-Item $state.settingsBackup $Settings -Force; "settings.ini restored ($Settings)" }
        Get-NewLog $state | Where-Object { $_ -match '\[SELFTEST\] video summary|\[CAPTURE\]|\[RECORD\] saved|ERROR|crash' }
        Remove-Item $StateFile
        "stopped pid=$($state.pid); captures in $($state.capture)"
    }
    default { $lines = Get-Content $PSCommandPath; $lines[1..([array]::IndexOf($lines, '#>') - 1)] }
}
