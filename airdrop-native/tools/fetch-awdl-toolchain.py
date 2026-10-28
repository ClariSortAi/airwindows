"""Acquire a pinned official LLVM archive; extract only native build tools locally."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import tarfile
import urllib.request

root = Path(__file__).resolve().parents[1]
out = root/'.cache/llvm-21.1.8'
out.mkdir(parents=True, exist_ok=True)
url = 'https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/clang%2Bllvm-21.1.8-x86_64-pc-windows-msvc.tar.xz'
digest = '749d22f565fcd5718dbed06512572d0e5353b502c03fe1f7f17ee8b8aca21a47'
expected_bytes = 942572476
archive = out/'official.tar.xz'
if not archive.exists():
    partial = out/'download.partial'
    with urllib.request.urlopen(urllib.request.Request(url,headers={'User-Agent':'AirWindows-build'}),timeout=60) as response, partial.open('xb') as dest:
        size = 0
        while block := response.read(1024*1024):
            size += len(block)
            if size>expected_bytes:raise RuntimeError('archive exceeds pinned size')
            dest.write(block)
    if size!=expected_bytes:raise RuntimeError('archive length mismatch')
    partial.rename(archive)
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
if archive.stat().st_size!=expected_bytes or sha(archive)!=digest:raise RuntimeError('official archive hash mismatch')
print('Official archive SHA-256 verified; extracting selected native tools.',flush=True)
files={}
with tarfile.open(archive,'r|xz') as bundle:
    for member in bundle:
        parts=PurePosixPath(member.name).parts
        if len(parts)<2:continue
        rel=PurePosixPath(*parts[1:])
        selected=(rel.parent==PurePosixPath('bin') and (rel.name in ('clang.exe','clang-cl.exe','lld.exe','lld-link.exe','ld.lld.exe') or rel.suffix=='.dll'))
        selected=selected or str(rel).startswith('lib/clang/21/include/') or rel.name.startswith(('LICENSE','NOTICE'))
        if not selected or member.isdir():continue
        if member.issym() or member.islnk():raise RuntimeError('selected archive member is a link: '+str(rel))
        if not member.isfile() or member.size>512*1024*1024 or '..' in rel.parts or rel.is_absolute():raise RuntimeError('invalid archive member')
        target=out/Path(*rel.parts)
        if not target.resolve().is_relative_to(out.resolve()):raise RuntimeError('unsafe extraction path')
        if target.exists():raise RuntimeError('fresh extraction required: '+str(target))
        target.parent.mkdir(parents=True,exist_ok=True)
        with bundle.extractfile(member) as source,target.open('xb') as dest:
            while chunk:=source.read(1024*1024):dest.write(chunk)
        files[str(rel)]=sha(target)
for name in ('bin/clang.exe','bin/lld-link.exe'):
    if name not in files:raise RuntimeError('required native tool missing: '+name)
(out/'source-receipt.json').write_text(json.dumps({'source_url':url,'archive_bytes':expected_bytes,'archive_sha256':digest,'files':files,'installed':False},indent=2)+'\n')
print('Local toolchain ready; no installer or system PATH change.',flush=True)
