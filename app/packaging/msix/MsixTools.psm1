# Shared by app/scripts/package-msix.ps1 and app/packaging/msix/probe/run-probe.ps1.
Set-StrictMode -Version Latest

$script:TemplatePath = Join-Path $PSScriptRoot 'AppxManifest.template.xml'
$script:AssetsPath = Join-Path $PSScriptRoot 'Assets'
$script:Tokens = @('IDENTITY_NAME','PUBLISHER','PUBLISHER_DISPLAY_NAME','DISPLAY_NAME','VERSION','RECEIVER_EXE',
    'RECEIVER_PARAMETERS','RECEIVER_ALIAS','IOSCTL_EXE','IOSCTL_ALIAS','IOSCTL_DISPLAY_NAME')

function Find-SdkTool([string] $Name) {
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $found = Get-ChildItem -LiteralPath $kits -Directory -ErrorAction Stop |
        Where-Object { $_.Name -match '^10\.0\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\$Name" } |
        Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $found) { throw "$Name not found under $kits\<version>\x64. Install the Windows 10/11 SDK." }
    return $found
}

function Test-PackageVersion([string] $Version, [switch] $AllowZeroMajor) {
    # Partner Center rule (app-package-requirements): four parts, 0..65535, fourth part 0, first part not 0.
    if ($Version -notmatch '^(\d+)\.(\d+)\.(\d+)\.(\d+)$') { throw "Version must have four numeric parts: $Version" }
    $parts = 1..4 | ForEach-Object { [int]$Matches[$_] }
    if ($parts | Where-Object { $_ -gt 65535 }) { throw "Each version part must be 0..65535: $Version" }
    if ($parts[3] -ne 0) { throw "The fourth version part is reserved for the Store and must be 0: $Version" }
    if ($parts[0] -eq 0 -and -not $AllowZeroMajor) {
        throw "The Store rejects a package version whose first part is 0 ($Version). Pass -AllowZeroMajor for a local test package."
    }
}

function New-AppxManifest([hashtable] $Values, [string] $Destination) {
    $text = Get-Content -LiteralPath $script:TemplatePath -Raw -Encoding utf8
    foreach ($token in $script:Tokens) {
        if (-not $Values.ContainsKey($token) -or [string]::IsNullOrWhiteSpace($Values[$token])) { throw "Manifest value missing: $token" }
        $text = $text.Replace("{{$token}}", [Security.SecurityElement]::Escape([string]$Values[$token]))
    }
    $left = [regex]::Matches($text, '\{\{[A-Z_]+\}\}') | ForEach-Object Value
    if ($left) { throw "Unfilled manifest tokens: $($left -join ', ')" }
    [xml]$text | Out-Null  # fails loudly on malformed XML
    [IO.File]::WriteAllText($Destination, $text, [Text.UTF8Encoding]::new($false))
}

function Copy-MsixAssets([string] $Layout) {
    $target = Join-Path $Layout 'Assets'
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    $assets = @(Get-ChildItem -LiteralPath $script:AssetsPath -Filter '*.png' -File)
    if ($assets.Count -eq 0) { throw "No assets in $script:AssetsPath. Run python app/scripts/make-store-assets.py." }
    $assets | Copy-Item -Destination $target
}

function Invoke-Tool([string] $Exe, [string[]] $Arguments, [string] $What) {
    $output = & $Exe @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "$What failed (exit $LASTEXITCODE)."
    }
    return $output
}

# resources.pri lets the shell pick the scale-, targetsize- and altform- variants
# behind the unqualified names (Assets\AppList.png) the manifest uses.
function New-ResourcesPri([string] $Layout, [string] $WorkDir) {
    $makepri = Find-SdkTool 'makepri.exe'
    $config = Join-Path $WorkDir 'priconfig.xml'
    Invoke-Tool $makepri @('createconfig', '/cf', $config, '/dq', 'en-US', '/pv', '10.0.0', '/o') 'makepri createconfig' | Out-Null
    Invoke-Tool $makepri @('new', '/pr', $Layout, '/cf', $config, '/mn', (Join-Path $Layout 'AppxManifest.xml'),
        '/of', (Join-Path $Layout 'resources.pri'), '/o') 'makepri new' | Out-Null
}

function New-MsixFile([string] $Layout, [string] $Package) {
    if (Test-Path -LiteralPath $Package) { throw "Output exists: $Package" }
    $makeappx = Find-SdkTool 'makeappx.exe'
    Invoke-Tool $makeappx @('pack', '/d', $Layout, '/p', $Package) 'makeappx pack' | Out-Null
}

function Get-PackageByName([string] $Name) {
    @(Get-AppxPackage -Name $Name)
}

function Unregister-PackageByName([string] $Name) {
    foreach ($package in Get-PackageByName $Name) {
        Write-Host "Removing $($package.PackageFullName)"
        Remove-AppxPackage -Package $package.PackageFullName
    }
}

Export-ModuleMember -Function Find-SdkTool, Test-PackageVersion, New-AppxManifest, Copy-MsixAssets, New-ResourcesPri,
    New-MsixFile, Get-PackageByName, Unregister-PackageByName
