"""Stage the exact unsigned candidate and validate its INF/catalog. Never install or sign."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path)
parser.add_argument('--kernel-build',type=Path,required=True)
parser.add_argument('--tools',type=Path,default=ROOT/'.cache/wdk-package-tools-26100')
args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
build=args.kernel_build.resolve();tools=args.tools.resolve()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
report={'stage':'unsigned_package_validation','installed':False,'signed':False,'hardware_tested':False,
        'airdrop_verified':False,'success':False,'commands':[]}
try:
    original=json.loads((build/'build-report.json').read_text())
    if original.get('entry')!='bringup' or not original.get('callbacks_wired'):
        raise RuntimeError('requires the actual bringup/radio kernel candidate')
    binary=build/'airwindows_mt7922_bringup.sys'
    if sha(binary)!=original['sha256']:raise RuntimeError('kernel binary differs from build receipt')
    for relative,expected in original['source_sha256'].items():
        if sha(ROOT/relative)!=expected:raise RuntimeError('kernel source differs from build receipt: '+relative)
    kit=json.loads((tools/'extracted-receipt.json').read_text())
    for relative,expected in kit['files'].items():
        path=(tools/relative).resolve()
        if not path.is_relative_to(tools) or sha(path)!=expected:raise RuntimeError('tool cache hash mismatch')
    inf=ROOT/'driver/airwindows_mt7922_bringup.inf'
    text=inf.read_text()
    exact='PCI\\VEN_14C3&DEV_0616&SUBSYS_223C14C3&REV_02'
    matches=[line.strip() for line in text.splitlines() if '=Install,PCI' in line]
    if matches!=['%Description%=Install,'+exact]:raise RuntimeError('INF hardware scope differs from exact adapter')
    package=out/'package';package.mkdir()
    shutil.copyfile(inf,package/inf.name);shutil.copyfile(binary,package/binary.name)
    report.update(kernel_build_receipt_sha256=sha(build/'build-report.json'),kernel_sha256=sha(binary),
                  inf_source_sha256=sha(inf),hardware_id=exact,
                  tool_cache_receipt_sha256=sha(tools/'extracted-receipt.json'))
    verifier=tools/'c/tools/10.0.26100.0/x64/infverif.exe'
    cataloger=tools/'c/bin/10.0.26100.0/x86/Inf2Cat.exe'
    def run(command):
        p=subprocess.run(list(map(str,command)),cwd=package,capture_output=True,text=True,timeout=90)
        report['commands'].append({'argv':list(map(str,command)),'exit_code':p.returncode,'output':p.stdout+p.stderr})
        print(p.stdout+p.stderr,flush=True)
        if p.returncode:raise RuntimeError('package validation failed')
    run([verifier,'/w','/v',package/inf.name])
    run([verifier,'/h','/v',package/inf.name])
    run([cataloger,'/driver:'+str(package),'/os:10_GE_X64','/verbose'])
    catalog=package/'airwindows_mt7922_bringup.cat'
    if not catalog.is_file() or not catalog.stat().st_size:raise RuntimeError('catalog was not generated')
    if sha(inf)!=report['inf_source_sha256'] or sha(binary)!=report['kernel_sha256']:
        raise RuntimeError('inputs changed during package generation')
    report['package_sha256']={p.name:sha(p) for p in sorted(package.iterdir()) if p.is_file()}
    report['builder_sha256']=sha(Path(__file__))
    report['success']=True
finally:
    (out/'package-report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
