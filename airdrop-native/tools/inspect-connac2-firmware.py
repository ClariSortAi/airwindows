"""Inspect a Connac2 firmware file without loading it into a device.

Layout: pinned mt76 mt76_connac_mcu.h, mt76_connac2_fw_trailer/region.
This reads file metadata, not device memory or connection keys.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def inspect(data):
    trailer_size, region_size = 36, 40
    if len(data) < trailer_size:
        raise ValueError('Firmware is shorter than its trailer')
    trailer = data[-trailer_size:]
    count = trailer[2]
    table_start = len(data) - trailer_size - count * region_size
    if not count or table_start < 0:
        raise ValueError('Invalid region count/table bounds')
    result = {
        'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data),
        'chip_id': trailer[0], 'eco_code': trailer[1],
        'format_version': trailer[3], 'format_flags': trailer[4],
        'firmware_version': trailer[7:17].rstrip(b'\0').decode('ascii'),
        'build_date': trailer[17:32].rstrip(b'\0').decode('ascii'),
        'trailer_crc': hex(struct.unpack_from('<I', trailer, 32)[0]),
        'crc_verified': False, 'region_table_offset': table_start, 'regions': [],
    }
    offset = 0
    for index in range(count):
        region = data[table_start + index * region_size:table_start + (index + 1) * region_size]
        crc, decompressed_len, block_size = struct.unpack_from('<III', region)
        address, length = struct.unpack_from('<II', region, 16)
        flags, kind = region[24:26]
        if length > table_start - offset:
            raise ValueError(f'Region {index} extends into the region table')
        payload = data[offset:offset + length]
        result['regions'].append({
            'index': index, 'file_offset': offset, 'length': length,
            'load_address': hex(address), 'features': hex(flags), 'type': kind,
            'encrypted_flag': bool(flags & 1), 'non_download_flag': bool(flags & 0x40),
            'override_address_flag': bool(flags & 0x20),
            'decompressed_length': decompressed_len, 'decompression_block_size': block_size,
            'decompressed_crc': hex(crc), 'sha256': hashlib.sha256(payload).hexdigest(),
        })
        offset += length
    result['unaccounted_payload_bytes'] = table_start - offset
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('firmware', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = inspect(args.firmware.read_bytes())
    report['source_path'] = str(args.firmware.resolve())
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps(report, indent=2))
