"""Version-pinned MediaTek firmware query. No radio mode changes or arbitrary IOCTLs."""
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import struct

DRIVER = Path(r'C:\Windows\System32\DriverStore\FileRepository\mtkwl6ex.inf_amd64_385620a2f6f13ea1\mtkwl6ex.sys')
SHA256 = '20112897f39752f1877ccbab9ca9b1fe6e5d554fe662b6d50973aa19f3c3d34a'


def main():
    if hashlib.sha256(DRIVER.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('Driver differs from the statically reviewed binary')
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
    kernel.CreateFileW.restype = w.HANDLE
    kernel.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, c.c_void_p, w.DWORD, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
    kernel.DeviceIoControl.restype = w.BOOL
    kernel.CloseHandle.argtypes = [w.HANDLE]
    kernel.CloseHandle.restype = w.BOOL
    handle = kernel.CreateFileW(r'\\.\MTKWLAN', 0xC0000000, 3, None, 3, 0, None)
    if handle == c.c_void_p(-1).value:
        raise c.WinError(c.get_last_error())
    try:
        # NdisCommonDeviceIoDispatch -> dispatch table -> FuncFWVer.
        # Verified entry 0x1401e410c, function 0x140014860, minimum output 100.
        oid = w.DWORD(0xFFFCF007)
        output = c.create_string_buffer(100)
        returned = w.DWORD()
        ok = kernel.DeviceIoControl(handle, 0x170002, c.byref(oid), 4, output, 100, c.byref(returned), None)
        error = 0 if ok else c.get_last_error()
        result = dict(query='firmware_version', driver_sha256=SHA256, success=bool(ok),
                      win32_error=error, bytes_returned=returned.value, radio_mode_changed=False)
        if ok:
            if returned.value != 100:
                raise RuntimeError('Unexpected response size')
            result['version_payload_hex'] = output.raw.hex()
            result['printable_runs'] = __import__('re').findall(r'[ -~]{4,}', output.raw.decode('ascii', errors='replace'))
        print(json.dumps(result, indent=2))
        return 0 if ok else 1
    finally:
        kernel.CloseHandle(handle)


if __name__ == '__main__':
    raise SystemExit(main())
