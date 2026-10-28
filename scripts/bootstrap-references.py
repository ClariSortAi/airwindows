"""Fetch pinned reference source. Never installs drivers or replaces existing work."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PINS = (
    ('airdrop-for-windows', 'https://github.com/Dy1an-Zou/Airdrop-for-Windows.git',
     '74606871b001bdba5e447f84a43bb96124fb495a'),
    ('mt76', 'https://github.com/openwrt/mt76.git',
     '467ae2e3f190baf737e15e318906da0dc7d0ab91'),
)

def git(folder, *args):
    return subprocess.run(['git', '-C', str(folder), *args], check=True,
                          capture_output=True, text=True).stdout.strip()

for name, url, commit in PINS:
    folder = ROOT/'airdrop-native/references'/name
    if folder.exists():
        if not (folder/'.git').exists():
            raise SystemExit(f'Refusing existing non-checkout: {folder}')
        if git(folder, 'rev-parse', 'HEAD') != commit or git(folder, 'status', '--porcelain'):
            raise SystemExit(f'Refusing mismatched or dirty checkout: {folder}')
    else:
        folder.mkdir(parents=True)
        git(folder, 'init', '--quiet')
        git(folder, 'remote', 'add', 'origin', url)
        git(folder, 'fetch', '--quiet', '--depth', '1', 'origin', commit)
        git(folder, 'checkout', '--quiet', '--detach', 'FETCH_HEAD')
        if git(folder, 'rev-parse', 'HEAD') != commit:
            raise SystemExit(f'Commit verification failed: {name}')
    print(f'{name}: {commit}')
