"""Build and run hardware-free Connac2 tests using the installed Windows toolchain."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--firmware-dir', type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
msvc = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133')
sdk = Path(r'C:\Program Files (x86)\Windows Kits\10')
env = os.environ.copy()
env['PATH'] = str(msvc / 'bin/Hostx64/x64') + os.pathsep + env['PATH']
env['INCLUDE'] = os.pathsep.join(map(str, [msvc / 'include', *[sdk / 'Include/10.0.19041.0' / d for d in ('ucrt', 'shared', 'um')]]))
env['LIB'] = os.pathsep.join(map(str, [msvc / 'lib/x64', *[sdk / 'Lib/10.0.19041.0' / d / 'x64' for d in ('ucrt', 'um')]]))
sources = [root / 'driver/mt7922' / name for name in ('tx.c', 'mcu.c', 'firmware.c', 'chip.c', 'dma.c', 'pcie_mcu.c', 'nic_caps.c', 'raw_rx.c', 'device_abi.c', 'mac_init.c', 'regulatory.c', 'clc_power.c', 'platform_acpi.c', 'channel_control.c', 'tx_power.c', 'monitor_context.c', 'radio_tx.c', 'rx_bridge.c', 'radio_session.c', 'radio_abi.c', 'test_tx.c', 'test_mcu.c', 'test_firmware.c', 'test_chip.c', 'test_dma.c', 'test_pcie_mcu.c', 'test_nic_caps.c', 'test_raw_rx.c', 'test_device_abi.c', 'test_regulatory.c', 'test_clc_power.c', 'test_platform_acpi.c', 'test_tx_power.c', 'test_rx_bridge.c', 'test_radio_abi.c')]
exe = out / 'test-mt7922-core.exe'
command = [str(msvc / 'bin/Hostx64/x64/cl.exe'), '/nologo', '/TC', '/W4', '/WX', '/MD',
           *map(str, sources), '/Fe:' + str(exe)]
build = subprocess.run(command, cwd=out, env=env, capture_output=True, text=True)
report = {'command': command, 'build_exit_code': build.returncode, 'build_output': build.stdout + build.stderr,
          'source_hashes': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                            for p in [*sources, *[root / 'driver/mt7922' / h for h in ('tx.h', 'mcu.h', 'firmware.h', 'chip.h', 'dma.h', 'pcie_mcu.h', 'nic_caps.h', 'raw_rx.h', 'device_abi.h', 'mac_init.h', 'regulatory.h', 'clc_power.h', 'platform_acpi.h', 'channel_control.h', 'tx_power.h', 'monitor_context.h', 'radio_tx.h', 'rx_bridge.h', 'radio_session.h', 'radio_abi.h', 'register_map.inc')]]}}
if build.returncode == 0:
    test_command = [str(exe)]
    if args.firmware_dir:
        images = [args.firmware_dir / n for n in ('WIFI_MT7922_patch_mcu_1_1_hdr.bin', 'WIFI_RAM_CODE_MT7922_1.bin')]
        report['firmware_file_hashes'] = {str(p.resolve()): hashlib.sha256(p.read_bytes()).hexdigest() for p in images}
        test_command += [str(p.resolve()) for p in images]
    test = subprocess.run(test_command, cwd=out, capture_output=True, text=True, timeout=20)
    report.update(test_exit_code=test.returncode, test_output=test.stdout + test.stderr,
                  binary_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
(out / 'test-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report, indent=2))
raise SystemExit(build.returncode or report.get('test_exit_code', 1))
