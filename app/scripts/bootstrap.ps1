<#
.SYNOPSIS
Fetches the two source trees AirWindows needs that are not in this repository, at pinned commits.

.DESCRIPTION
Run once on a fresh checkout, then run scripts\build.ps1. Nothing is installed
system-wide: everything stays under this project.

  third_party\UxPlay   UxPlay sources (AirPlay protocol engine)
  .deps\vcpkg          vcpkg, which builds OpenSSL, libplist and FFmpeg

The libraries themselves are built by the first run of scripts\build.ps1, as
listed in vcpkg.json. That first build compiles OpenSSL and FFmpeg from source
and takes a while; later builds reuse the result.
#>

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root '.deps'

$UxPlayCommit = 'ec203121355220dd90278a369eeabb0b8ba4f14a'
$VcpkgCommit = 'e456309491fd875487bf02b8a0dca76c1b1b00cc'   # OpenSSL 3.6.5, libplist 2.8.0, FFmpeg 9.0.2

function Invoke-Checked([scriptblock] $command, [string] $what) {
    & $command
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)." }
}

function Get-PinnedRepo([string] $url, [string] $commit, [string] $dir) {
    if (Test-Path (Join-Path $dir '.git')) {
        $have = (git -C $dir rev-parse HEAD).Trim()
        if ($have -eq $commit) { Write-Host "$dir is already at $commit"; return }
    } else {
        New-Item -ItemType Directory -Force $dir | Out-Null
        Invoke-Checked { git -C $dir init --quiet } "git init $dir"
        Invoke-Checked { git -C $dir remote add origin $url } "git remote add $url"
    }
    Invoke-Checked { git -C $dir fetch --quiet --depth 1 origin $commit } "git fetch $url $commit"
    Invoke-Checked { git -C $dir checkout --quiet FETCH_HEAD } "git checkout $commit"
    Write-Host "$dir checked out at $commit"
}

New-Item -ItemType Directory -Force $deps | Out-Null

# 1. UxPlay
Get-PinnedRepo 'https://github.com/FDH2/UxPlay' $UxPlayCommit (Join-Path $root 'third_party\UxPlay')

# 2. vcpkg. The ports it carries at this commit fix the library versions.
$vcpkg = Join-Path $deps 'vcpkg'
Get-PinnedRepo 'https://github.com/microsoft/vcpkg' $VcpkgCommit $vcpkg
if (-not (Test-Path (Join-Path $vcpkg 'vcpkg.exe'))) {
    Invoke-Checked { & (Join-Path $vcpkg 'bootstrap-vcpkg.bat') -disableMetrics } 'vcpkg bootstrap'
}

Write-Host ''
Write-Host 'Bootstrap complete. Next: .\scripts\build.ps1'
