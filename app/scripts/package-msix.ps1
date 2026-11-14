<#
.SYNOPSIS
Builds the PhonePort MSIX layout and .msix for the Microsoft Store, or registers
the layout for a local test.

.DESCRIPTION
Writes into a new -OutDir:
  layout\              the loose package (AppxManifest.xml, resources.pri, Assets, binaries, notices)
  PhonePort-<version>-x64.msix    the unsigned package to upload in Partner Center
The Store signs MSIX uploads, so no certificate is used. -Register registers
layout\ (needs Developer Mode, no elevation). -Unregister removes every installed
package named -IdentityName and does nothing else. See app/packaging/msix/README.md.

.EXAMPLE
./app/scripts/package-msix.ps1 -OutDir app/dist/msix-1.0.0
#>
param(
    [string] $ExePath,
    [string] $IosctlPath,
    [string] $IdentityName = 'GraniteAi.PhonePort.Dev',
    [string] $Publisher = 'CN=PhonePort Dev',
    [string] $PublisherDisplayName = 'GraniteAi, LLC',
    [string] $DisplayName = 'PhonePort',
    [string] $Version,
    [string] $SourceUrl,
    [string] $OutDir,
    [switch] $AllowZeroMajor,
    [switch] $Register,
    [switch] $Unregister
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$repo = Split-Path -Parent $root
Import-Module (Join-Path $root 'packaging\msix\MsixTools.psm1') -Force

if ($Unregister) {
    if ($Register -or $OutDir) { throw '-Unregister runs alone; it does not package.' }
    Unregister-PackageByName $IdentityName
    if (Get-PackageByName $IdentityName) { throw "$IdentityName is still registered." }
    Write-Host "$IdentityName is not registered."
    return
}

if (-not $ExePath) { $ExePath = Join-Path $root 'build\Release\bin\PhonePort.exe' }
$ExePath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ExePath)
if (-not (Test-Path -LiteralPath $ExePath)) { throw "Receiver not found: $ExePath. Build it first or pass -ExePath." }
if (-not $IosctlPath) { $IosctlPath = Join-Path (Split-Path -Parent $ExePath) 'iosctl.exe' }
$IosctlPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($IosctlPath)
if (-not (Test-Path -LiteralPath $IosctlPath)) { throw "iosctl not found: $IosctlPath. Build it first or pass -IosctlPath." }
$productVersion = (Get-Item -LiteralPath $ExePath).VersionInfo.ProductVersion
if (-not $productVersion) { throw 'Receiver has no product version.' }
if ((Get-Item -LiteralPath $IosctlPath).VersionInfo.ProductVersion -ne $productVersion) {
    throw "iosctl version does not match the receiver ($productVersion). Build the matching iosctl."
}
if (-not $Version) {
    if ($productVersion -notmatch '^(\d+)\.(\d+)\.(\d+)') { throw "Cannot read x.y.z from product version $productVersion" }
    $Version = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
}
Test-PackageVersion $Version -AllowZeroMajor:$AllowZeroMajor
if (-not $SourceUrl) { $SourceUrl = "https://github.com/ClariSortAi/airwindows/releases/tag/v$productVersion" }

if (-not $OutDir) { throw 'Pass -OutDir <new folder>; output is never overwritten.' }
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
if (Test-Path -LiteralPath $OutDir) { throw "Output exists; choose a new -OutDir: $OutDir" }

# Notices: the same files package.ps1 ships in the ZIP.
$share = Join-Path $root '.deps\vcpkg_installed\x64-windows-static\share'
$licenses = [ordered]@{
    'FFmpeg-LGPL-2.1.txt' = Join-Path $share 'ffmpeg\copyright'
    'OpenSSL-Apache-2.0.txt' = Join-Path $share 'openssl\copyright'
    'libplist-LGPL-2.1.txt' = Join-Path $share 'libplist\copyright'
    'llhttp-MIT.txt' = Join-Path $root 'third_party\UxPlay\lib\llhttp\LICENSE-MIT'
    'iphone-use-MIT.txt' = Join-Path $repo 'native-ble-prototype\LICENSE-iphone-use'
}
foreach ($source in $licenses.Values) {
    if (-not (Test-Path -LiteralPath $source)) { throw "License text missing: $source" }
}

$layout = Join-Path $OutDir 'layout'
$package = Join-Path $OutDir "PhonePort-$Version-x64.msix"
New-Item -ItemType Directory -Path (Join-Path $layout 'licenses') -Force | Out-Null
Copy-Item -LiteralPath $ExePath -Destination (Join-Path $layout 'PhonePort.exe')
Copy-Item -LiteralPath $IosctlPath -Destination (Join-Path $layout 'iosctl.exe')
foreach ($name in @('FOR-AGENTS.md', 'AGENTS.md', 'CLAUDE.md', 'requirements-agent.txt')) {
    Copy-Item -LiteralPath (Join-Path $repo $name) -Destination $layout
}
$agentApp = Join-Path $layout 'app'
New-Item -ItemType Directory -Path (Join-Path $agentApp 'scripts') -Force | Out-Null
foreach ($name in @('README.md', 'NATIVE-CONTROL.md', 'CLAUDE-CONTROL.md', 'FRAME-INTERFACE.md', 'THIRD-PARTY.md', 'LICENSE')) {
    Copy-Item -LiteralPath (Join-Path $root $name) -Destination $agentApp
}
foreach ($name in @('observe-phone.py', 'airwindows_frames.py')) {
    Copy-Item -LiteralPath (Join-Path $root "scripts\$name") -Destination (Join-Path $agentApp 'scripts')
}
$helperDocs = Join-Path $agentApp 'tools\iosctl'
New-Item -ItemType Directory -Path $helperDocs -Force | Out-Null
foreach ($name in @('README.md', 'preflight.ps1', 'targets.example.json', 'DEMO-TAKE-A-PHOTO.md')) {
    Copy-Item -LiteralPath (Join-Path $root "tools\iosctl\$name") -Destination $helperDocs
}
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination (Join-Path $layout 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'THIRD-PARTY.md') -Destination (Join-Path $layout 'THIRD-PARTY.txt')
foreach ($name in $licenses.Keys) {
    Copy-Item -LiteralPath $licenses[$name] -Destination (Join-Path $layout "licenses\$name")
}
# GPL-3 section 6: say where the corresponding source for this exact build is. A GitHub
# release page holds PhonePort-<version>-source.zip (package.ps1 -Release), which bundles the
# upstream library sources; a tag or tree address holds the repository at that version.
$sourceLines = if ($SourceUrl -match '/releases/') {
    @('The complete corresponding source for this build, including the linked',
      'upstream library sources, is published as',
      "PhonePort-$productVersion-source.zip at:", $SourceUrl)
} else {
    @('The complete corresponding source for this build is published at:', $SourceUrl,
      'app/scripts/bootstrap.ps1 there fetches UxPlay and vcpkg at pinned commits;',
      'vcpkg fetches the FFmpeg, OpenSSL and libplist sources the build links.')
}
@(
    "PhonePort $productVersion (package version $Version)",
    '',
    'PhonePort is licensed under the GNU General Public License version 3 (LICENSE.txt).'
) + $sourceLines + @(
    '',
    'Third-party notices: THIRD-PARTY.txt and the licenses folder.'
) | Set-Content -LiteralPath (Join-Path $layout 'SOURCE.txt') -Encoding utf8

New-AppxManifest @{
    IDENTITY_NAME = $IdentityName; PUBLISHER = $Publisher; PUBLISHER_DISPLAY_NAME = $PublisherDisplayName
    DISPLAY_NAME = $DisplayName; VERSION = $Version
    RECEIVER_EXE = 'PhonePort.exe'; RECEIVER_PARAMETERS = '--frames'; RECEIVER_ALIAS = 'phoneport.exe'
    IOSCTL_EXE = 'iosctl.exe'; IOSCTL_ALIAS = 'iosctl.exe'; IOSCTL_DISPLAY_NAME = 'iosctl'
} (Join-Path $layout 'AppxManifest.xml')
Copy-MsixAssets $layout
New-ResourcesPri $layout $OutDir
New-MsixFile $layout $package

$fake = $IdentityName -eq 'GraniteAi.PhonePort.Dev' -or $Publisher -eq 'CN=PhonePort Dev'
Write-Host "Layout:  $layout"
Write-Host "Package: $package"
Write-Host "Identity: Name=$IdentityName Publisher=$Publisher Version=$Version (x64)"
if ($fake) {
    Write-Warning 'Placeholder identity: Partner Center rejects this upload. Pass -IdentityName, -Publisher and -PublisherDisplayName from Product identity in Partner Center.'
} else {
    Write-Host "Upload $package on the Packages page of the Partner Center submission."
}

if ($Register) {
    Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml')
    $installed = Get-PackageByName $IdentityName
    if (-not $installed) { throw "Registration reported success but $IdentityName is not installed." }
    Write-Host "Registered $($installed[0].PackageFullName) from $layout"
    Write-Host "Remove it with: ./app/scripts/package-msix.ps1 -Unregister -IdentityName $IdentityName"
}
