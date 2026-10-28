"""Native Windows diagnostic client. Never installs, signs or rebinds a driver.

status is read-only. validate/self-test never open a device. boot uses only the
pinned firmware files, reports boot status, then explicitly stops before close.
The driver also stops a boot-owned session when its last handle closes.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import struct

DEVICE = r'\\.\AirWindowsMt7922Bringup'
IOCTL_STATUS, IOCTL_BOOT, IOCTL_STOP = 0x226400, 0x22e404, 0x22a408
MAX_IMAGE = 8 * 1024 * 1024
PINS = {
    'WIFI_MT7922_patch_mcu_1_1_hdr.bin': '6c22dc4bdf97c0ff678841f183e543b14d7244d834343a1cb15d1f09fa178a72',
    'WIFI_RAM_CODE_MT7922_1.bin': '3e3a04e330713706fe111456cc807b6c398fa750ca18f707e0eb3e682ce0cfed',
}


def encode_boot(patch, ram):
    if not 0 < len(patch) <= MAX_IMAGE or not 0 < len(ram) <= MAX_IMAGE:
        raise ValueError('Empty or oversized firmware image')
    return struct.pack('<IIII', 0x3242544d, 1, len(patch), len(ram)) + patch + ram


def load_images(folder):
    images = []
    evidence = {}
    for name, expected in PINS.items():
        path = folder / name
        if not 0 < path.stat().st_size <= MAX_IMAGE:
            raise ValueError(f'Invalid image size: {name}')
        # Bound the read even if the file changes after stat. Kernel hashes the
        # copied request again before any register access.
        with path.open('rb') as f:
            data = f.read(MAX_IMAGE + 1)
        digest = hashlib.sha256(data).hexdigest()
        if len(data) > MAX_IMAGE or digest != expected:
            raise ValueError(f'Firmware SHA-256 does not match reviewed pin: {name}')
        images.append(data)
        evidence[name] = dict(bytes=len(data), sha256=digest)
    return encode_boot(*images), evidence


def decode_status(data):
    if len(data) != 128:
        raise ValueError('Wrong status length')
    values = struct.unpack('<32I', data)
    if values[:3] != (0x3253544d, 1, 128):
        raise ValueError('Wrong status magic/version/size')
    flags = values[3]
    if flags & ~255 or values[25] or values[26]:
        raise ValueError('Unexpected ABI flags or readiness claim')
    if any(data[108:]) or values[13] or any(data[86:88]):
        raise ValueError('Nonzero reserved status bytes')
    prepared = bool(flags & 1)
    if prepared and values[22:25] != (0x061614c3, 0x223c14c3, 2):
        raise ValueError('Wrong adapter identity')
    def signed(value):
        return c.c_int32(value).value
    return dict(
        generation=values[7], operation_status=f'0x{values[4]:08x}', operation_succeeded=not bool(values[4] & 0x80000000),
        core_error=signed(values[5]), stop_error=signed(values[6]), mcu_fault=signed(values[10]),
        chip_start_error=signed(values[11]), chip_stop_error=signed(values[12]),
        prepared=prepared, powered=bool(flags & 2), removed=bool(flags & 4), boot_attempted=bool(flags & 8),
        firmware_ready=bool(flags & 16), capabilities_valid=bool(flags & 32),
        dma_published=bool(flags & 64), dma_quiescent=bool(flags & 128),
        firmware_bytes=values[8], firmware_regions=values[9],
        capabilities=dict(present=values[14], bands=values[15], nss=values[16], chainmask=values[17],
                          chip_low=values[18], chip_high=values[19], mac=':'.join(f'{b:02x}' for b in data[80:86]))
        if flags & 32 else None,
        radio_ready=False, airdrop_ready=False)


class Device:
    def __init__(self, write=False):
        self.dll = c.WinDLL('kernel32', use_last_error=True)
        self.dll.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
        self.dll.CreateFileW.restype = w.HANDLE
        self.dll.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, c.c_void_p, w.DWORD, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
        self.dll.DeviceIoControl.restype = w.BOOL
        self.dll.CloseHandle.argtypes = [w.HANDLE]
        self.dll.CloseHandle.restype = w.BOOL
        self.handle = self.dll.CreateFileW(DEVICE, 0xc0000000 if write else 0x80000000, 0, None, 3, 0, None)
        if self.handle == c.c_void_p(-1).value:
            raise c.WinError(c.get_last_error())

    def control(self, code, data=b''):
        source = c.create_string_buffer(data) if data else None
        output = c.create_string_buffer(128)
        returned = w.DWORD()
        if not self.dll.DeviceIoControl(self.handle, code, source, len(data), output, 128, c.byref(returned), None):
            raise c.WinError(c.get_last_error())
        if returned.value != 128:
            raise ValueError('Incomplete device status')
        return decode_status(output.raw)

    def close(self):
        if self.handle is not None:
            self.dll.CloseHandle(self.handle)
            self.handle = None


def boot_session(device, request):
    result = {'airdrop_ready': False}
    try:
        result['boot'] = device.control(IOCTL_BOOT, request)
    except (OSError, ValueError) as exc:
        result['boot_error'] = str(exc)
    finally:
        try:
            result['stop'] = device.control(IOCTL_STOP)
        except (OSError, ValueError) as exc:
            result['stop_error'] = str(exc)
    boot, stop = result.get('boot', {}), result.get('stop', {})
    success = all(boot.get(k) for k in ('operation_succeeded', 'firmware_ready', 'capabilities_valid'))
    success = success and all(stop.get(k) for k in ('operation_succeeded', 'dma_quiescent'))
    return result, bool(success)


def self_test():
    assert encode_boot(b'P', b'RR') == b'MTB2\x01\0\0\0\x01\0\0\0\x02\0\0\0PRR'
    for patch, ram in ((b'', b'R'), (b'P', b'')):
        try:
            encode_boot(patch, ram)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted empty image')
    status = bytearray(128)
    struct.pack_into('<4I', status, 0, 0x3253544d, 1, 128, 3)
    struct.pack_into('<3I', status, 88, 0x061614c3, 0x223c14c3, 2)
    decoded = decode_status(status)
    assert decoded['prepared'] and decoded['powered'] and not decoded['airdrop_ready']
    bad = [status[:n] for n in range(128)] + [status + b'\0']
    for offset in (0, 4, 8, 88, 92, 96, 100, 104, 108):
        altered = bytearray(status)
        altered[offset] ^= 1
        bad.append(altered)
    for value in bad:
        try:
            decode_status(value)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted malformed status')
    struct.pack_into('<I', status, 20, 0xfffffffb)
    assert decode_status(status)['core_error'] == -5
    class FakeDevice:
        def __init__(self, fail_boot, fail_stop):
            self.fail_boot, self.fail_stop, self.calls = fail_boot, fail_stop, []
        def control(self, code, data=b''):
            self.calls.append(code)
            if code == IOCTL_BOOT:
                if self.fail_boot:
                    raise ValueError('boot failed')
                return dict(operation_succeeded=True, firmware_ready=True, capabilities_valid=True)
            assert code == IOCTL_STOP
            if self.fail_stop:
                raise ValueError('stop failed')
            return dict(operation_succeeded=True, dma_quiescent=True)
    for fail_boot, fail_stop in ((False, False), (True, False), (False, True), (True, True)):
        fake = FakeDevice(fail_boot, fail_stop)
        result, success = boot_session(fake, b'synthetic')
        assert fake.calls == [IOCTL_BOOT, IOCTL_STOP]
        assert ('boot_error' in result) == fail_boot and ('stop_error' in result) == fail_stop
        assert success == (not fail_boot and not fail_stop)
    return dict(client_tests='passed', malformed_status_cases=len(bad), cleanup_cases=4, device_access=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('status', 'validate', 'boot', 'stop', 'self-test'), nargs='?', default='status')
    parser.add_argument('--firmware-dir', type=Path)
    args = parser.parse_args()
    if args.command == 'self-test':
        print(json.dumps(self_test()))
        return 0
    request = b''
    if args.command in ('validate', 'boot'):
        if args.firmware_dir is None:
            parser.error('--firmware-dir is required for validate/boot')
        request, images = load_images(args.firmware_dir)
        if args.command == 'validate':
            print(json.dumps(dict(firmware_files=images, request_bytes=len(request), device_access=False, airdrop_ready=False)))
            return 0
    device = None
    try:
        device = Device(write=args.command != 'status')
        if args.command == 'boot':
            # This client exposes only the diagnostic ABI, not the driver's
            # newer radio IOCTLs. Always stop; cleanup is a second fence.
            result, success = boot_session(device, request)
            print(json.dumps(result))
            return 0 if success else 1
        status = device.control(IOCTL_STOP if args.command == 'stop' else IOCTL_STATUS)
        print(json.dumps(dict(device_open=True, status=status, airdrop_ready=False)))
        return 0 if status['operation_succeeded'] else 1
    except OSError as exc:
        print(json.dumps(dict(device_open=device is not None, win32_error=exc.winerror, error=str(exc), airdrop_ready=False)))
        return 1
    finally:
        if device:
            device.close()


if __name__ == '__main__':
    raise SystemExit(main())
