"""Fetch two pinned upstream MT7922 images for offline analysis; never load them."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import urllib.request

COMMIT = 'd0039fd797d7776d89f00c22c45754914a96706e'
BASE = 'https://kernel.googlesource.com/pub/scm/linux/kernel/git/firmware/linux-firmware/+/'
FILES = {
    'mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin': '9d990796d74e1658b7bc5699ac2ea52d741f3c48',
    'mediatek/WIFI_RAM_CODE_MT7922_1.bin': 'c18871c47e6a57e9755a2d42e1bb8b21ac28ef36',
    'LICENSES/LICENCE.mediatek': '6886c61bab1bec4c1a653abd2324adac4f296fcb',
    'WHENCE': '2ac590890a7c1754bace581502f8bcb30c8cc72b',
}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    report = {'repository_commit': COMMIT, 'purpose': 'offline_reference_only',
              'loaded_on_hardware': False, 'driver_firmware_pins_changed': False,
              'files': [], 'complete': False}
    try:
        for name, expected in FILES.items():
            # Fetch immutable blobs from the upstream project. The kernel
            # Gitiles mirror intermittently returns 503 for binary/text blobs.
            url = ('https://gitlab.com/api/v4/projects/kernel-firmware%2Flinux-firmware/'
                   'repository/blobs/' + expected)
            with urllib.request.urlopen(url, timeout=30) as response:
                body = response.read(16 * 1024 * 1024 + 1)
            if len(body) > 16 * 1024 * 1024:
                raise ValueError('Download exceeds bound: ' + name)
            document = json.loads(body)
            if document['encoding'] != 'base64':
                raise ValueError('Unexpected encoding: ' + name)
            data = base64.b64decode(document['content'], validate=True)
            blob = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
            if expected and blob != expected:
                raise ValueError('Pinned Git blob mismatch: ' + name)
            destination = args.output / Path(name).name
            with destination.open('xb') as stream:
                stream.write(data)
            report['files'].append({'name': destination.name, 'url': url, 'size': len(data),
                                    'commit_path': BASE + COMMIT + '/' + name,
                                    'git_blob_sha1': blob, 'pinned_blob_verified': bool(expected),
                                    'sha256': hashlib.sha256(data).hexdigest()})
        report['complete'] = True
    finally:
        report['fetch_script_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        (args.output / 'source-receipt.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
