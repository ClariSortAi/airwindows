<#
.SYNOPSIS
Collects the built program and its license texts into a folder, and optionally makes the release archives.

.DESCRIPTION
Without -Release, writes only the runnable folder:

  <OutDir>\AirWindows\                 the program, README.txt and licenses

With -Release, also writes next to it:

  AirWindows-<version>-win-x64.zip   the folder above, zipped
  AirWindows-<version>-source.zip    this project, UxPlay, and the upstream
                                     source archives of OpenSSL, libplist and
                                     FFmpeg with the vcpkg ports that build them
  SHA256SUMS.txt                     checksums of the two archives

The source archive is there because the program is GPL-3.0-or-later: whoever
receives the binary must be able to get the source of everything linked into it.

.PARAMETER Config
Which build to package. Defaults to Release.

.PARAMETER OutDir
Destination folder. Defaults to dist inside the project.

.PARAMETER Release
Also create the two zip archives and the checksum file.
#>
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string] $Config = 'Release',
    [string] $OutDir,
    [switch] $Release
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\$Config\bin\AirWindows.exe"
if (-not (Test-Path $exe)) { throw "No build found at $exe. Run scripts\build.ps1 first." }
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }
# Resolve a relative path against PowerShell's location. .NET keeps its own
# current directory, which is not always the same one.
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)

$version = (Get-Item $exe).VersionInfo.ProductVersion
if (-not $version) { throw "$exe carries no product version." }

$share = Join-Path $root '.deps\vcpkg_installed\x64-windows-static\share'
$uxplay = Join-Path $root 'third_party\UxPlay'
$licenses = [ordered] @{
    'FFmpeg-LGPL-2.1.txt'     = Join-Path $share 'ffmpeg\copyright'
    'OpenSSL-Apache-2.0.txt'  = Join-Path $share 'openssl\copyright'
    'libplist-LGPL-2.1.txt'   = Join-Path $share 'libplist\copyright'
    'llhttp-MIT.txt'          = Join-Path $uxplay 'lib\llhttp\LICENSE-MIT'
}
foreach ($source in $licenses.Values) {
    if (-not (Test-Path $source)) { throw "License text not found: $source" }
}

# 1. The runnable folder.
$appDir = Join-Path $OutDir 'AirWindows'
if (Test-Path $appDir) { Remove-Item $appDir -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $appDir 'licenses') | Out-Null
Copy-Item $exe $appDir
Copy-Item (Join-Path $root 'packaging\README.txt') $appDir
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $appDir 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD-PARTY.md') (Join-Path $appDir 'THIRD-PARTY.txt')
foreach ($name in $licenses.Keys) {
    Copy-Item $licenses[$name] (Join-Path $appDir "licenses\$name")
}
Write-Host "Packaged AirWindows $version to $appDir"

if (-not $Release) { return }

# 2. Binary archive.
$binaryZip = Join-Path $OutDir "AirWindows-$version-win-x64.zip"
if (Test-Path $binaryZip) { Remove-Item $binaryZip -Force }
Compress-Archive -Path $appDir -DestinationPath $binaryZip

# 3. Source archive, staged in a temporary folder.
$downloads = Join-Path $root '.deps\vcpkg\downloads'
$upstream = foreach ($pattern in 'openssl-openssl-openssl-*.tar.gz', 'libimobiledevice-libplist-*.tar.gz', 'ffmpeg-ffmpeg-*.tar.gz') {
    $found = @(Get-ChildItem (Join-Path $downloads $pattern) -ErrorAction SilentlyContinue)
    if ($found.Count -ne 1) { throw "Expected exactly one $pattern in $downloads, found $($found.Count)." }
    $found[0]
}

$stage = Join-Path ([IO.Path]::GetTempPath()) "AirWindows-source-$([guid]::NewGuid().ToString('N'))"
$top = Join-Path $stage "AirWindows-$version-source"
try {
    New-Item -ItemType Directory -Force $top | Out-Null
    # robocopy exit codes below 8 mean success.
    robocopy $root $top /E /XD .deps build dist third_party .git /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying the project failed (robocopy exit code $LASTEXITCODE)." }
    robocopy $uxplay (Join-Path $top 'third_party\UxPlay') /E /XD .git /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying UxPlay failed (robocopy exit code $LASTEXITCODE)." }

    $upstreamDir = Join-Path $top 'upstream-sources'
    New-Item -ItemType Directory -Force $upstreamDir | Out-Null
    $upstream | Copy-Item -Destination $upstreamDir
    foreach ($port in 'openssl', 'libplist') {
        Copy-Item (Join-Path $root ".deps\vcpkg\ports\$port") (Join-Path $upstreamDir "vcpkg-ports\$port") -Recurse
    }
    $vcpkgCommit = (git -C (Join-Path $root '.deps\vcpkg') rev-parse HEAD).Trim()
    $uxplayCommit = (git -C $uxplay rev-parse HEAD).Trim()
    @"
Upstream sources for AirWindows $version

The archives in this folder are the unmodified upstream sources that vcpkg
downloaded and compiled into AirWindows.exe:

$(($upstream | ForEach-Object { "  $($_.Name)" }) -join "`n")

vcpkg applies the patches in a library's port before building it. The ports
used for OpenSSL and libplist are copied to vcpkg-ports here. The FFmpeg port
is the project's own, in vcpkg-ports\ffmpeg at the top of this archive.

vcpkg commit:  $vcpkgCommit
UxPlay commit: $uxplayCommit (in third_party\UxPlay, unmodified)

To rebuild, see "Building from source" in README.md.
"@ | Set-Content (Join-Path $upstreamDir 'README.txt') -Encoding utf8

    $sourceZip = Join-Path $OutDir "AirWindows-$version-source.zip"
    if (Test-Path $sourceZip) { Remove-Item $sourceZip -Force }
    Compress-Archive -Path $top -DestinationPath $sourceZip
} finally {
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
}

# 4. Checksums, in the format sha256sum reads.
$sums = foreach ($file in $binaryZip, $sourceZip) {
    '{0}  {1}' -f (Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path $file -Leaf)
}
$sums | Set-Content (Join-Path $OutDir 'SHA256SUMS.txt') -Encoding ascii

Write-Host 'Release files:'
foreach ($file in $binaryZip, $sourceZip, (Join-Path $OutDir 'SHA256SUMS.txt')) {
    Write-Host ('  {0}  ({1:N1} MB)' -f $file, ((Get-Item $file).Length / 1MB))
}
