<# Package a build and its notices; -Release adds matching source and checksums. #>
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string] $Config = 'Release',
    [ValidateSet('AirWindowsNative', 'AirWindows')] [string] $Target = 'AirWindowsNative',
    [string] $OutDir,
    [switch] $Release
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent $root
$exe = Join-Path $root "build\$Config\bin\$Target.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "Build $Target first with scripts/build.ps1 -Target $Target." }
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
$version = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
if (-not $version) { throw 'Executable has no product version.' }
$share = Join-Path $root '.deps\vcpkg_installed\x64-windows-static\share'
$uxplay = Join-Path $root 'third_party\UxPlay'
$backend = Join-Path $repo 'native-ble-prototype'
$licenses = [ordered]@{
    'FFmpeg-LGPL-2.1.txt' = Join-Path $share 'ffmpeg\copyright'
    'OpenSSL-Apache-2.0.txt' = Join-Path $share 'openssl\copyright'
    'libplist-LGPL-2.1.txt' = Join-Path $share 'libplist\copyright'
    'llhttp-MIT.txt' = Join-Path $uxplay 'lib\llhttp\LICENSE-MIT'
    'iphone-use-MIT.txt' = Join-Path $backend 'LICENSE-iphone-use'
}
foreach ($source in $licenses.Values) {
    if (-not (Test-Path -LiteralPath $source)) { throw "License text missing: $source" }
}
$appDir = Join-Path $OutDir 'AirWindows'
$binaryZip = Join-Path $OutDir "AirWindows-$version-win-x64.zip"
$sourceZip = Join-Path $OutDir "AirWindows-$version-source.zip"
$sumsPath = Join-Path $OutDir 'SHA256SUMS.txt'
foreach ($targetPath in @($appDir, $binaryZip, $sourceZip, $sumsPath)) {
    if (Test-Path -LiteralPath $targetPath) { throw "Output exists; choose a new -OutDir: $targetPath" }
}
$upstream = @()
if ($Release) {
    $downloads = Join-Path $root '.deps\vcpkg\downloads'
    foreach ($pattern in @('openssl-openssl-openssl-*.tar.gz', 'libimobiledevice-libplist-*.tar.gz', 'ffmpeg-ffmpeg-*.tar.gz')) {
        $found = @(Get-ChildItem -Path (Join-Path $downloads $pattern) -File)
        if ($found.Count -ne 1) { throw "Expected one source archive for $pattern; found $($found.Count)." }
        $upstream += $found[0]
    }
}
New-Item -ItemType Directory -Path (Join-Path $appDir 'licenses') -Force | Out-Null
Copy-Item -LiteralPath $exe -Destination $appDir
Copy-Item -LiteralPath (Join-Path $root 'packaging\README.txt') -Destination $appDir
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $appDir 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'THIRD-PARTY.md') -Destination (Join-Path $appDir 'THIRD-PARTY.txt')
foreach ($name in $licenses.Keys) {
    Copy-Item -LiteralPath $licenses[$name] -Destination (Join-Path $appDir "licenses\$name")
}
Write-Host "Packaged $Target $version to $appDir"
if (-not $Release) { return }
Compress-Archive -LiteralPath $appDir -DestinationPath $binaryZip
$stage = Join-Path $OutDir ('.source-stage-' + [guid]::NewGuid().ToString('N'))
$top = Join-Path $stage "AirWindows-$version-source"
try {
    $sourceApp = Join-Path $top 'app'
    New-Item -ItemType Directory -Path $sourceApp -Force | Out-Null
    Get-ChildItem -LiteralPath $root -File | Where-Object { $_.Extension -in @('.md','.txt','.json') -or $_.Name -eq 'LICENSE' } |
        Copy-Item -Destination $sourceApp
    foreach ($dir in @('src','compat','scripts','tests','packaging','vcpkg-ports')) {
        robocopy (Join-Path $root $dir) (Join-Path $sourceApp $dir) /E /XD __pycache__ .pytest_cache /XF *.pyc /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Source copy failed: $dir" }
    }
    $sourceBackend = Join-Path $top 'native-ble-prototype'
    New-Item -ItemType Directory -Path $sourceBackend -Force | Out-Null
    foreach ($name in @('native_hid_backend.cpp','native_hid_backend.h','absolute_buttons.h','battery_level.h','discovery_window.h','test_discovery_window.cpp','README.md','LICENSE-iphone-use')) {
        Copy-Item -LiteralPath (Join-Path $backend $name) -Destination $sourceBackend
    }
    Copy-Item -LiteralPath (Join-Path $repo 'requirements-dev.txt') -Destination $top
    Copy-Item -LiteralPath (Join-Path $repo 'FOR-AGENTS.md') -Destination $top
    New-Item -ItemType Directory -Path (Join-Path $top 'scripts') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo 'scripts\check.ps1') -Destination (Join-Path $top 'scripts')
    @('# AirWindows source', '', 'Build from the app directory with scripts/bootstrap.ps1 and scripts/build.ps1 -Target AirWindowsNative.', 'See app/README.md for prerequisites, app/LICENSE for licensing and app/THIRD-PARTY.md for notices.', 'Keep app/ and native-ble-prototype/ beside each other. The archive includes the linked upstream sources.', 'The separate AirDrop research tree is not part of this application bundle.') |
        Set-Content -LiteralPath (Join-Path $top 'README.md') -Encoding utf8
    robocopy $uxplay (Join-Path $sourceApp 'third_party\UxPlay') /E /XD .git /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw 'UxPlay source copy failed.' }
    $upstreamDir = Join-Path $sourceApp 'upstream-sources'
    New-Item -ItemType Directory -Path (Join-Path $upstreamDir 'vcpkg-ports') -Force | Out-Null
    $upstream | Copy-Item -Destination $upstreamDir
    foreach ($port in @('openssl','libplist')) {
        Copy-Item -LiteralPath (Join-Path $root ".deps\vcpkg\ports\$port") -Destination (Join-Path $upstreamDir "vcpkg-ports\$port") -Recurse
    }
    $vcpkgCommit = (git -C (Join-Path $root '.deps\vcpkg') rev-parse HEAD).Trim()
    $uxplayCommit = (git -C $uxplay rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the UxPlay source commit.' }
    $uxplayCommit | Set-Content -LiteralPath (Join-Path $sourceApp 'third_party\UxPlay\AIRWINDOWS-UPSTREAM-COMMIT.txt') -Encoding ascii
    @("vcpkg: $vcpkgCommit", "UxPlay: $uxplayCommit", 'FFmpeg overlay: app/vcpkg-ports/ffmpeg. OpenSSL/libplist ports: this directory.') |
        Set-Content -LiteralPath (Join-Path $upstreamDir 'README.txt') -Encoding utf8
    Compress-Archive -LiteralPath $top -DestinationPath $sourceZip
} finally {
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    $outputPrefix = [IO.Path]::GetFullPath($OutDir).TrimEnd('\') + '\'
    if (-not $resolvedStage.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing cleanup outside output directory.' }
    if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
}
$sums = foreach ($file in @($binaryZip, $sourceZip)) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant(), (Split-Path -Leaf $file)
}
$sums | Set-Content -LiteralPath $sumsPath -Encoding ascii
Write-Host "Release archives and SHA256SUMS.txt are in $OutDir"
