"""Compile/link an unsigned KMDF identity or firmware bring-up candidate; never install.

The default harness exposes only PCI identity. --entry bringup wires device
resources and explicit firmware boot/stop, but supplies no RF/AirDrop operation.
Pinned Microsoft headers/libraries are cached for subsequent native driver builds.
"""
from pathlib import Path
import argparse
import base64
import hashlib
import json
import os
import re
import subprocess
import time
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '10.0.26100.1'
KIT = '10.0.26100.0'
PACKAGES = {
    'microsoft.windows.wdk.x64': '5aHxX9Jh7tv9wqfXR2taMxP0LOwz1t/2UwAx70Z3uyKlkkYmNOxCTQ3rod1mShsJJ63e6Nmq/53gPJkcLOQIAg==',
    'microsoft.windows.sdk.cpp.x64': 'EhG+TCUZWAQr2U8CyFBPhIZedtUphVxpRl6fKOSOQvEQYelZ1FslrkmAbNBHqbp80eUOTb1Z7VkIEU/56c2Pdw==',
    'microsoft.windows.sdk.cpp': 'ZveRWpfQKpduSRyI9WxRFQ5BpU9XZ+K2BGQ6lZup//PiH+KPFIGFd/QqrvFjh+isrlOOiJ5cNF6IKuZDCh1E7w==',
}
PREFIXES = (f'c/include/{KIT}/km/', f'c/include/{KIT}/shared/', f'c/include/{KIT}/ucrt/',
            'c/include/wdf/kmdf/1.33/', f'c/lib/{KIT}/km/x64/', 'c/lib/wdf/kmdf/x64/1.33/')
CORE = ('tx', 'mcu', 'firmware', 'chip', 'dma', 'pcie_mcu', 'nic_caps', 'raw_rx', 'device_abi', 'mac_init', 'regulatory', 'clc_power', 'platform_acpi', 'channel_control', 'tx_power', 'monitor_context', 'radio_tx', 'rx_bridge', 'radio_session', 'radio_abi', 'windows_platform')


def acquire_cache(cache: Path, rate: float):
    receipt = cache / 'cache-receipt.json'
    if receipt.exists():
        data = json.loads(receipt.read_text(encoding='utf-8'))
        if data['version'] != VERSION or data['packages'] != PACKAGES:
            raise RuntimeError('Unexpected cached kit identity')
        for relative, expected in data['files'].items():
            path = (cache / relative).resolve()
            if not path.is_relative_to(cache) or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
                raise RuntimeError(f'Cached kit file differs: {relative}')
        return data
    cache.mkdir(parents=True, exist_ok=True)
    files = {}
    expanded = 0
    for package, expected in PACKAGES.items():
        url = f'https://api.nuget.org/v3-flatcontainer/{package}/{VERSION}/{package}.{VERSION}.nupkg'
        archive = cache / (package + '.nupkg')
        digest = hashlib.sha512()
        received = 0
        start = time.monotonic()
        print(f'Acquiring pinned {package}', flush=True)
        with urllib.request.urlopen(url, timeout=60) as response, archive.open('wb') as output:
            while chunk := response.read(1024 * 1024):
                received += len(chunk)
                if received > 512 * 1024**2:
                    raise RuntimeError('Package exceeds bounded download size')
                output.write(chunk)
                digest.update(chunk)
                delay = received / (rate * 1024**2) - (time.monotonic() - start)
                if delay > 0:
                    time.sleep(delay)
        if base64.b64encode(digest.digest()).decode() != expected:
            raise RuntimeError(f'Pinned SHA-512 mismatch: {package}')
        with zipfile.ZipFile(archive) as z:
            for member in z.infolist():
                name = member.filename.lower()
                if member.is_dir() or not (name.startswith(PREFIXES) or name == 'license.txt'):
                    continue
                # Preserve each package's license separately.
                relative = f'licenses/{package}.txt' if name == 'license.txt' else member.filename
                target = (cache / relative).resolve()
                if not target.is_relative_to(cache):
                    raise RuntimeError('Archive target escapes cache')
                expanded += member.file_size
                if expanded > 2 * 1024**3:
                    raise RuntimeError('Extracted kit exceeds 2 GiB bound')
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(member) as source:
                    contents = source.read()
                sha = hashlib.sha256(contents).hexdigest()
                if relative in files and files[relative] != sha:
                    raise RuntimeError(f'Conflicting kit files: {relative}')
                target.write_bytes(contents)
                files[relative] = sha
        archive.unlink()
    data = dict(version=VERSION, packages=PACKAGES, files=files)
    receipt.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--entry', choices=('identity', 'bringup'), default='identity')
    p.add_argument('--kit-cache', type=Path, default=ROOT / '.cache/wdk-10.0.26100.1')
    p.add_argument('--download-mib-per-second', type=float, default=8)
    p.add_argument('--msvc', type=Path, default=Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133'))
    args = p.parse_args()
    if not 0 < args.download_mib_per_second <= 100:
        p.error('Download rate must be >0 and <=100 MiB/s')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    entry = ROOT / ('driver/probe.c' if args.entry == 'identity' else 'driver/mt7922/windows_device.c')
    sources = [ROOT / 'driver/mt7922' / (name + '.c') for name in CORE] + [entry]
    inputs = sources + [ROOT / 'driver/mt7922' / (name + '.h') for name in CORE]
    inputs += [ROOT / 'driver/mt7922/register_map.inc', Path(__file__).resolve()]
    report = dict(stage='started', entry=args.entry, callbacks_wired=args.entry == 'bringup', installed=False, signed=False, hardware_binding=False,
                  radio_ready=False, airdrop_ready=False, commands=[],
                  source_sha256={str(f.relative_to(ROOT)): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs})
    try:
        # Independent guard against a transcription error in the kernel pins.
        expected_pins = {
            'patch_sha': '6c22dc4bdf97c0ff678841f183e543b14d7244d834343a1cb15d1f09fa178a72',
            'ram_sha': '3e3a04e330713706fe111456cc807b6c398fa750ca18f707e0eb3e682ce0cfed',
            'radio_patch_sha': '7fc9075d6d54b31e0539734614ca971fa52caf41b642a3000c8a4501591a4d98',
            'radio_ram_sha': 'd100e5e04c324fc8da6302112d1e0b4fac952286447d2419eabcd8fb0a1f5b93',
        }
        platform_source = (ROOT / 'driver/mt7922/windows_platform.c').read_text(encoding='utf-8')
        pins = {name: bytes(int(x, 16) for x in re.findall(r'0x[0-9a-f]+', value)).hex()
                for name, value in re.findall(r'uint8_t (patch_sha|ram_sha|radio_patch_sha|radio_ram_sha)\[32\]=\{([^}]+)', platform_source)}
        if pins != expected_pins:
            raise RuntimeError('Kernel firmware digest constants differ from reviewed image pins')
        report['firmware_sha256_pins'] = pins
        cache = args.kit_cache.resolve()
        kit = acquire_cache(cache, args.download_mib_per_second)
        report['kit_packages'] = kit['packages']
        report['kit_receipt_sha256'] = hashlib.sha256((cache / 'cache-receipt.json').read_bytes()).hexdigest()
        includes = [args.msvc / 'include', cache / f'c/Include/{KIT}/km', cache / f'c/Include/{KIT}/shared',
                    cache / f'c/Include/{KIT}/ucrt', cache / 'c/Include/wdf/kmdf/1.33']
        binpath = args.msvc / 'bin/Hostx64/x64'
        env = dict(os.environ, VSLANG='1033')
        env['PATH'] = str(binpath) + os.pathsep + env['PATH']

        def run(command):
            result = subprocess.run(list(map(str, command)), cwd=out, env=env, capture_output=True)
            output = (result.stdout + result.stderr).decode('utf-8', errors='replace')
            report['commands'].append(dict(command=list(map(str, command)), exit_code=result.returncode, output=output))
            print(output, end='', flush=True)
            result.check_returncode()
            return output

        objects = []
        for source in sources:
            obj = out / (source.stem + '.obj')
            run([binpath / 'cl.exe', '/nologo', '/c', '/TC', '/W4', '/WX', '/kernel', '/Zl', '/GS', '/guard:cf',
                 '/D_AMD64_', '/D_WIN64', '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A00000C',
                 '/DKMDF_VERSION_MAJOR=1', '/DKMDF_VERSION_MINOR=33', '/external:W0',
                 *['/external:I' + str(x) for x in includes], '/Fo' + str(obj), source])
            objects.append(obj)
        binary = out / ('mt7922_core_identity_harness.sys' if args.entry == 'identity' else 'airwindows_mt7922_bringup.sys')
        run([binpath / 'link.exe', '/driver:wdm', '/release', '/subsystem:native', '/entry:FxDriverEntry',
             '/nodefaultlib', '/machine:x64', '/integritycheck', '/guard:cf', '/dynamicbase', '/nxcompat',
             '/opt:noref', '/map:' + str(out / 'kernel.map'), '/out:' + str(binary),
             '/libpath:' + str(cache / f'c/Lib/{KIT}/km/x64'), '/libpath:' + str(cache / 'c/Lib/wdf/kmdf/x64/1.33'),
             *objects, 'ntoskrnl.lib', 'hal.lib', 'wmilib.lib', 'WdfDriverEntry.lib', 'WdfLdr.lib', 'BufferOverflowFastFailK.lib', 'cng.lib'])
        imports = run([binpath / 'dumpbin.exe', '/imports', binary])
        (out / 'imports.txt').write_text(imports, encoding='utf-8')
        # The full core must remain linked even though the identity harness
        # intentionally does not call it. Never let dead-code removal fake this check.
        maptext = (out / 'kernel.map').read_text(encoding='utf-8', errors='replace')
        required = ('mt2_tx_submit', 'mt2_fw_boot', 'mt2_chip_start', 'mt2_chip_stop',
                    'mt2_pcie_query_nic', 'mt2_raw_rx_record', 'mt2_rx_poll', 'mt2_mac_initialize', 'mt2_pcie_query_regdomain',
                    'mt2_pcie_query_hw_type', 'mt2_pcie_apply_clc', 'mt2_platform_read', 'mt2_platform_policy_build', 'mt2_channel_apply', 'mt2_channel_monitor', 'mt2_tx_power_apply', 'mt2_pcie_query_pse', 'mt2_monitor_context_start', 'mt2_monitor_context_stop', 'mt2_pcie_bind_tx', 'mt2_radio_tx_start', 'mt2_radio_tx_submit', 'mt2_radio_tx_poll', 'mt2_radio_tx_stop', 'mt2_rx_bridge_record', 'mt2_radio_session_start', 'mt2_radio_session_poll', 'mt2_radio_session_stop', 'mt2_radio_request', 'mt2_radio_session_address',
                    'mt2_windows_prepare', 'mt2_windows_boot', 'mt2_windows_radio_start', 'mt2_windows_leave', 'mt2_windows_release')
        if args.entry == 'bringup':
            required += ('Mt2Prepare', 'Mt2D0Exit', 'Mt2Removed', 'Mt2Cleanup', 'Mt2Control')
        if any(name not in maptext for name in required):
            raise RuntimeError('Kernel map is missing an expected core symbol')
        report['linked_core_symbols'] = list(required)
        report['sha256'] = hashlib.sha256(binary.read_bytes()).hexdigest()
        report['stage'] = ('linked_unsigned_core_in_identity_harness' if args.entry == 'identity'
                           else 'linked_unsigned_firmware_bringup')
    except Exception as exc:
        report.update(stage='failed', error=str(exc))
        raise
    finally:
        (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('stage', 'sha256', 'installed', 'radio_ready', 'airdrop_ready')}))


if __name__ == '__main__':
    main()
