"""Compile an unsigned diagnostic candidate. Never install, sign or bind it."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
REFERENCE = ROOT / 'references/airdrop-for-windows'
PIN = '74606871b001bdba5e447f84a43bb96124fb495a'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--msvc', type=Path, required=True, help='MSVC toolset directory')
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if subprocess.check_output(['git', '-C', str(REFERENCE), 'rev-parse', 'HEAD'], text=True).strip() != PIN:
        raise RuntimeError('Reference revision differs from reviewed revision')
    if subprocess.check_output(['git', '-C', str(REFERENCE), 'status', '--porcelain'], text=True).strip():
        raise RuntimeError('Reference checkout is modified')
    # Reuse the reviewed helper only for bounded Microsoft kit acquisition.
    spec = importlib.util.spec_from_file_location('kit', REFERENCE / 'driver/mt7925/build_driver.py')
    kit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(kit)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = ROOT / 'driver/probe.c'
    report = dict(installed=False, signed=False, radio_ready=False, airdrop_ready=False,
                  source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), reference_commit=PIN)
    try:
        with tempfile.TemporaryDirectory(prefix='airwindows-mt7922-kit-') as temp:
            tmp = Path(temp)
            report['packages'] = kit.acquire_kit(tmp)
            includes = [args.msvc / 'include', tmp / f'c/Include/{kit.KIT}/km',
                        tmp / f'c/Include/{kit.KIT}/shared', tmp / f'c/Include/{kit.KIT}/ucrt',
                        tmp / 'c/Include/wdf/kmdf/1.33']
            cl = args.msvc / 'bin/Hostx64/x64/cl.exe'
            link = args.msvc / 'bin/Hostx64/x64/link.exe'
            obj = out / 'probe.obj'
            exe = out / 'airwindows_mt7922_probe.sys'
            commands = [
                [str(cl), '/nologo', '/c', '/W4', '/WX', '/kernel', '/Zl', '/GS', '/guard:cf',
                 '/D_AMD64_', '/D_WIN64', '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A00000C',
                 '/DKMDF_VERSION_MAJOR=1', '/DKMDF_VERSION_MINOR=33', '/external:W0',
                 *['/external:I' + str(x) for x in includes], '/Fo' + str(obj), str(source)],
                [str(link), '/driver:wdm', '/release', '/subsystem:native', '/entry:FxDriverEntry',
                 '/nodefaultlib', '/machine:x64', '/integritycheck', '/guard:cf', '/dynamicbase',
                 '/nxcompat', '/out:' + str(exe), '/libpath:' + str(tmp / f'c/Lib/{kit.KIT}/km/x64'),
                 '/libpath:' + str(tmp / 'c/Lib/wdf/kmdf/x64/1.33'), str(obj), 'ntoskrnl.lib',
                 'hal.lib', 'wmilib.lib', 'WdfDriverEntry.lib', 'WdfLdr.lib', 'BufferOverflowFastFailK.lib']]
            report['commands'] = []
            for command in commands:
                result = subprocess.run(command, capture_output=True)
                output = (result.stdout + result.stderr).decode('utf-8', errors='replace')
                report['commands'].append(dict(command=command, exit_code=result.returncode, output=output))
                print(output, flush=True)
                result.check_returncode()
            report['sha256'] = hashlib.sha256(exe.read_bytes()).hexdigest()
            report['stage'] = 'compiled_unsigned_probe_only'
    except Exception as exc:
        report['stage'] = 'failed'
        report['error'] = str(exc)
        raise
    finally:
        (out / 'build-report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
