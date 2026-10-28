"""Read the diagnostic device if already installed; never loads a driver."""
import ctypes as c
from ctypes import wintypes as w
import json
import struct


def main():
    dll = c.WinDLL('kernel32', use_last_error=True)
    dll.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
    dll.CreateFileW.restype = w.HANDLE
    dll.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, c.c_void_p, w.DWORD, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
    dll.DeviceIoControl.restype = w.BOOL
    dll.CloseHandle.argtypes = [w.HANDLE]
    dll.CloseHandle.restype = w.BOOL
    handle = dll.CreateFileW(r'\\.\AirWindowsMt7922Probe', 0x80000000, 3, None, 3, 0, None)
    if handle == c.c_void_p(-1).value:
        code = c.get_last_error()
        print(json.dumps(dict(probe_open=False, win32_error=code, airdrop_ready=False)))
        return 1
    try:
        buffer = c.create_string_buffer(16)
        returned = w.DWORD()
        ioctl = (0x22 << 16) | (1 << 14) | (0x800 << 2)
        if not dll.DeviceIoControl(handle, ioctl, None, 0, buffer, 16, c.byref(returned), None):
            raise c.WinError(c.get_last_error())
        if returned.value != 16:
            raise RuntimeError('Unexpected diagnostic response size')
        version, vendor, device, subvendor, subsystem, ready = struct.unpack('<IHHHHI', buffer.raw)
        if version != 1 or (vendor, device, subvendor, subsystem) != (0x14c3, 0x0616, 0x14c3, 0x223c):
            raise RuntimeError('Unexpected diagnostic identity')
        print(json.dumps(dict(probe_open=True, pci_identity_read=bool(ready),
                              pci_id=f'{vendor:04x}:{device:04x}', airdrop_ready=False)))
        return 0 if ready else 1
    finally:
        dll.CloseHandle(handle)


if __name__ == '__main__':
    raise SystemExit(main())
