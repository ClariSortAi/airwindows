param([string]$Output = (Join-Path $PSScriptRoot ('artifacts\host-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.json')))
$ErrorActionPreference = 'Stop'
$devices = @(Get-PnpDevice -Class Net -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_14C3&DEV_0616&SUBSYS_223C14C3*' })
if ($devices.Count -ne 1) { throw 'Expected exactly one matching MT7922; no changes made.' }
$device = $devices[0]
$signed = Get-CimInstance Win32_PnPSignedDriver | Where-Object DeviceID -eq $device.InstanceId
$service = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_Service).Data
$image = (Get-ItemProperty -LiteralPath "HKLM:\SYSTEM\CurrentControlSet\Services\$service").ImagePath
$path = $image -replace '^\\SystemRoot', $env:SystemRoot
$report = [ordered]@{
    capturedUtc = [DateTime]::UtcNow.ToString('o')
    device = $device | Select-Object FriendlyName,InstanceId,Status
    driver = $signed | Select-Object DriverVersion,InfName,DriverProviderName
    driverBinary = $path
    driverSha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    secureBootRegistry = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State').UEFISecureBootEnabled
    os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption,BuildNumber
    wirelessCapabilities = (& netsh wlan show wirelesscapabilities | Out-String)
    networkAdapters = @(Get-NetAdapter -Physical | Select-Object Name,InterfaceDescription,Status)
    constraints = @('native Windows', 'built-in MT7922', 'iOS native AirDrop picker')
    driverChanged = $false
    radioBackendReady = $false
    airdropReceivingVerified = $false
}
if (Test-Path -LiteralPath $Output) { throw 'Output exists; choose a new evidence path.' }
New-Item -ItemType Directory -Force (Split-Path -Parent $Output) | Out-Null
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Output -Encoding utf8
Write-Output $Output
