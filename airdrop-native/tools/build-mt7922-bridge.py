"""Build the native AWDL adapter and run device-free contract/pipe tests."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--awdld', type=Path, help='Optional built AWDL executable for the device-free process integration test')
args = p.parse_args()
root = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
vc = Path(r'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133')
sdk = Path(r'C:\Program Files (x86)\Windows Kits\10')
compiler = vc/'bin/Hostx64/x64/cl.exe'
env = os.environ.copy()
env['PATH'] = str(compiler.parent)+os.pathsep+env['PATH']
env['INCLUDE'] = os.pathsep.join(map(str,[vc/'include',*[sdk/'Include/10.0.19041.0'/d for d in ('ucrt','shared','um')]]))
env['LIB'] = os.pathsep.join(map(str,[vc/'lib/x64',*[sdk/'Lib/10.0.19041.0'/d/'x64' for d in ('ucrt','um')]]))
ref = root/'references/airdrop-for-windows/radio'
files = [*sorted((root/'radio').glob('*.h')), *sorted((root/'radio').glob('*.cpp')),Path(__file__).resolve()]
files += [ref/n for n in ('mt7925_codec.h','mt7925_device.h','mt7925_bridge.h','pipe_server.h','radio_link.h','frame_capture.h','mt7925_rx_diagnostics.h')]
files += sorted((root/'driver/mt7922').glob('*.h'))
def hashes():
    return {str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}
report = {'hardware_access':False,'rf_started':False,'installed':False,'source_sha256':hashes(),'commands':[],'success':False}
def run(cmd, expected=0):
    result = subprocess.run(list(map(str,cmd)),cwd=out,env=env,capture_output=True,text=True,timeout=90)
    report['commands'].append({'argv':list(map(str,cmd)),'exit_code':result.returncode,'expected_exit_code':expected,'output':result.stdout+result.stderr})
    print(result.stdout+result.stderr,flush=True)
    if result.returncode!=expected:raise RuntimeError('build/test command failed')
try:
    names=['test_adapter','test_pipe','winawdl_mt7922']
    if args.awdld:
        args.awdld=args.awdld.resolve(strict=True)
        report['awdl_process']={'path':str(args.awdld),'sha256':hashlib.sha256(args.awdld.read_bytes()).hexdigest()}
        names.append('test_awdl_process')
    for name in names:
        target=out/(name+'.exe')
        run([compiler,'/nologo','/std:c++20','/EHsc','/W4','/WX','/MT','/utf-8','/I'+str(ref),
             '/Fe:'+str(target),'/Fo:'+str(out/(name+'.obj')),root/'radio'/(name+'.cpp'),'/link','bcrypt.lib','advapi32.lib'])
        run([target,'--help'] if name=='winawdl_mt7922' else [target,args.awdld] if name=='test_awdl_process' else [target])
    # All reject before pipe creation/device opening. No --run trial is invoked.
    run([out/'winawdl_mt7922.exe','--country','US'],1)
    run([out/'winawdl_mt7922.exe','--run','--run'],1)
    if report['source_sha256']!=hashes():raise RuntimeError('sources changed during build/test')
    report['artifacts']={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in out.glob('*.exe')}
    report['success']=True
finally:
    (out/'verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
