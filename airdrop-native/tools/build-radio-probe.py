"""Build the bounded native stock-driver diagnostic using the installed SDK."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

root = Path(__file__).resolve().parent
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=False)
msvc = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133')
sdk = Path(r'C:\Program Files (x86)\Windows Kits\10')
env = os.environ.copy()
env['PATH'] = str(msvc / 'bin/Hostx64/x64') + os.pathsep + env['PATH']
env['INCLUDE'] = os.pathsep.join(map(str, [msvc / 'include', *[sdk / 'Include/10.0.19041.0' / d for d in ('ucrt', 'shared', 'um')]]))
env['LIB'] = os.pathsep.join(map(str, [msvc / 'lib/x64', *[sdk / 'Lib/10.0.19041.0' / d / 'x64' for d in ('ucrt', 'um')]]))
source = root / 'stock-radio-probe.cpp'
command = [str(msvc / 'bin/Hostx64/x64/cl.exe'), '/nologo', '/W4', '/WX', '/EHsc', '/std:c++17', '/MD', '/D_CRT_SECURE_NO_WARNINGS', str(source), '/Fe:' + str(out / 'stock-radio-probe.exe'), '/Fo:' + str(out / 'stock-radio-probe.obj'), '/link', 'advapi32.lib', 'wlanapi.lib', 'bcrypt.lib']
result = subprocess.run(command, env=env, capture_output=True, text=True)
report = dict(command=command, exit_code=result.returncode, output=result.stdout + result.stderr, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())
exe = out / 'stock-radio-probe.exe'
if exe.exists(): report['binary_sha256'] = hashlib.sha256(exe.read_bytes()).hexdigest()
(out / 'build-report.json').write_text(json.dumps(report, indent=2))
print(report['output'])
raise SystemExit(result.returncode)
