param([string] $OutDir = (Join-Path $PSScriptRoot '..\..\build\Release\bin'))
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw '.NET Framework 4.x C# compiler is required.' }
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
$output = Join-Path $OutDir 'iosctl.exe'
$cmake = Get-Content -LiteralPath (Join-Path $PSScriptRoot '..\..\CMakeLists.txt') -Raw
$version = [regex]::Match($cmake, 'project\(AirWindows VERSION ([0-9.]+)').Groups[1].Value
$suffix = [regex]::Match($cmake, 'set\(AIRWINDOWS_VERSION_SUFFIX "([a-z0-9.\-]*)"\)').Groups[1].Value
if (-not $version) { throw 'Cannot read the application version.' }
$versionSource = Join-Path $OutDir 'iosctl-version.cs'
@("[assembly:System.Reflection.AssemblyVersion(`"$version.0`")]", "[assembly:System.Reflection.AssemblyFileVersion(`"$version.0`")]", "[assembly:System.Reflection.AssemblyInformationalVersion(`"$version$suffix`")]") |
    Set-Content -LiteralPath $versionSource -Encoding utf8
$sources = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.cs' | Select-Object -ExpandProperty FullName)
& $compiler /nologo /optimize /platform:x64 /warnaserror /target:exe "/out:$output" "/win32manifest:$PSScriptRoot\iosctl.manifest" /r:System.Drawing.dll /r:System.Web.Extensions.dll $sources $versionSource
if ($LASTEXITCODE -ne 0) { throw 'iosctl build failed.' }
Write-Host "Built $output"
