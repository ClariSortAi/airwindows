"""Decode the exact reviewed driver's WDI dispatch tables from retained REA bytes."""
import json
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[1]
session = root / 'artifacts/kernel-session'
expected = '20112897f39752f1877ccbab9ca9b1fe6e5d554fe662b6d50973aa19f3c3d34a'
tables = []
for request, kind, base, stride, count in [
    (278, 'property', 0x1401c8ef0, 106, 29),
    (279, 'task', 0x1401c85b0, 118, 20),
]:
    source = session / f'{request}-wdi-table.result.json'
    evidence = json.loads(json.loads(source.read_text())['content'][0]['text'])
    if evidence['subject']['digest']['sha256'] != expected:
        raise ValueError('Driver digest does not match reviewed layout')
    result = evidence['normalized_result']
    raw = bytes.fromhex(result['bytes_hex'])
    if int(result['address'], 16) != base or len(raw) != stride * count:
        raise ValueError('Incomplete or unexpected table read')
    entries = []
    for index in range(count):
        record = raw[index * stride:(index + 1) * stride]
        entries.append({
            'index': index, 'address': hex(base + index * stride),
            'oid': hex(struct.unpack_from('<I', record)[0]),
            'name': record[4:68].split(b'\0', 1)[0].decode('ascii'),
            # Names for these slots follow the recovered dispatch code, not PDBs.
            'query_handler': hex(struct.unpack_from('<Q', record, 70)[0]),
            'set_or_method_handler': hex(struct.unpack_from('<Q', record, 78)[0]),
            'tail_hex': record[68:].hex(),
        })
    tables.append({'kind': kind, 'base': hex(base), 'stride': stride,
                   'source': str(source.relative_to(root)),
                   'evidence_id': evidence['evidence_id'], 'entries': entries})
report = {'driver_sha256': expected, 'tables': tables,
          'boundary': 'Static table membership; does not establish application reachability.'}
output = root / 'artifacts/wdi-tables-decoded.json'
output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'tables': len(tables), 'entries': sum(len(t['entries']) for t in tables),
                  'output': str(output)}))
