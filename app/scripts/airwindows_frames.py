"""Read opt-in AirWindows decoded frames through its local, user-only pipe."""
from __future__ import annotations

import ctypes
from ctypes import wintypes as w
import io
import json
import struct
import time
from pathlib import Path

from PIL import Image


def validate_metadata(meta: dict, now_ms: int, expected_pid: int, *, allow_selftest=False):
    if not meta.get("ok"):
        raise ValueError(f"AirWindows: {meta.get('error', 'frame unavailable')}")
    if meta.get("protocol") != 1 or meta.get("format") != "ppm":
        raise ValueError("Unsupported AirWindows frame protocol")
    if meta.get("source") != "airplay" and not (allow_selftest and meta.get("source") == "selftest"):
        raise ValueError("Expected a real AirPlay source")
    if meta.get("receiver_pid") != expected_pid:
        raise ValueError("Receiver identity changed")
    if not isinstance(meta.get("instance"), str) or not meta["instance"].isdigit():
        raise ValueError("Missing receiver instance")
    for key in ("generation", "sequence", "width", "height", "image_bytes", "received_tick_ms", "decoded_tick_ms"):
        if type(meta.get(key)) is not int or meta[key] <= 0:
            raise ValueError(f"Invalid {key}")
    if meta["width"] > 4096 or meta["height"] > 4096 or not 0 < meta["image_bytes"] <= 51_000_000:
        raise ValueError("Frame exceeds limits")
    for key in ("received_tick_ms", "decoded_tick_ms"):
        if not 0 <= now_ms - meta[key] <= 1000:
            raise ValueError("AirPlay frame is stale or has an invalid clock")
    if meta["received_tick_ms"] > meta["decoded_tick_ms"]:
        raise ValueError("Decode timestamp precedes packet receipt")


class _Overlapped(ctypes.Structure):
    _fields_ = [("internal", ctypes.c_size_t), ("internal_high", ctypes.c_size_t),
                ("offset", w.DWORD), ("offset_high", w.DWORD), ("event", w.HANDLE)]


class AirPlayReader:
    def __init__(self, pid: int, *, allow_selftest=False):
        self.pid = int(pid)
        if self.pid <= 0:
            raise ValueError("Receiver PID must be positive")
        self.allow_selftest = allow_selftest
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        k = self.kernel
        k.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
        k.CreateFileW.restype = w.HANDLE
        k.CreateEventW.argtypes = [w.LPVOID, w.BOOL, w.BOOL, w.LPCWSTR]
        k.CreateEventW.restype = w.HANDLE
        k.WaitNamedPipeW.argtypes = [w.LPCWSTR, w.DWORD]
        k.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
        k.CloseHandle.argtypes = [w.HANDLE]
        k.CancelIoEx.argtypes = [w.HANDLE, ctypes.POINTER(_Overlapped)]
        k.GetOverlappedResult.argtypes = [w.HANDLE, ctypes.POINTER(_Overlapped), ctypes.POINTER(w.DWORD), w.BOOL]
        k.GetNamedPipeServerProcessId.argtypes = [w.HANDLE, ctypes.POINTER(w.ULONG)]
        for function in (k.ReadFile, k.WriteFile):
            function.argtypes = [w.HANDLE, w.LPVOID, w.DWORD, ctypes.POINTER(w.DWORD), ctypes.POINTER(_Overlapped)]
        k.GetTickCount64.restype = ctypes.c_uint64

    def _io(self, handle, data, *, write=False, deadline):
        k = self.kernel
        count = w.DWORD()
        ov = _Overlapped()
        ov.event = k.CreateEventW(None, True, False, None)
        if not ov.event:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            operation = k.WriteFile if write else k.ReadFile
            ok = operation(handle, data, len(data), ctypes.byref(count), ctypes.byref(ov))
            if not ok:
                error = ctypes.get_last_error()
                if error != 997:  # ERROR_IO_PENDING
                    raise ctypes.WinError(error)
                wait_ms = max(0, int((deadline - time.monotonic()) * 1000))
                if k.WaitForSingleObject(ov.event, wait_ms) != 0:
                    k.CancelIoEx(handle, ctypes.byref(ov))
                    k.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(count), True)
                    raise TimeoutError("AirWindows frame read timed out")
                if not k.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(count), False):
                    raise ctypes.WinError(ctypes.get_last_error())
            return count.value
        finally:
            k.CloseHandle(ov.event)

    def _read(self, handle, length, deadline):
        result = bytearray()
        while len(result) < length:
            if time.monotonic() >= deadline:
                raise TimeoutError("AirWindows frame read timed out")
            buffer = ctypes.create_string_buffer(min(65536, length - len(result)))
            count = self._io(handle, buffer, deadline=deadline)
            if not count:
                raise OSError("AirWindows closed an incomplete frame")
            result.extend(buffer.raw[:count])
        return bytes(result)

    def grab(self, *, timeout=3.0):
        k = self.kernel
        deadline = time.monotonic() + timeout
        pipe = rf"\\.\pipe\AirWindows.Frames.{self.pid}"
        handle = k.CreateFileW(pipe, 0xC0000000, 0, None, 3, 0x40000000, None)
        if handle == ctypes.c_void_p(-1).value:
            error = ctypes.get_last_error()
            if error == 231 and k.WaitNamedPipeW(pipe, int(timeout * 1000)):
                handle = k.CreateFileW(pipe, 0xC0000000, 0, None, 3, 0x40000000, None)
            if handle == ctypes.c_void_p(-1).value:
                raise OSError("AirWindows frame pipe unavailable; start this receiver with --frames")
        try:
            server_pid = w.ULONG()
            if not k.GetNamedPipeServerProcessId(handle, ctypes.byref(server_pid)) or server_pid.value != self.pid:
                raise ValueError("Unexpected process owns the frame pipe")
            size, = struct.unpack("<I", self._read(handle, 4, deadline))
            if not 1 <= size <= 8192:
                raise ValueError("Invalid frame metadata length")
            meta = json.loads(self._read(handle, size, deadline))
            if not isinstance(meta, dict):
                raise ValueError("Invalid frame metadata")
            validate_metadata(meta, k.GetTickCount64(), self.pid, allow_selftest=self.allow_selftest)
            payload = self._read(handle, meta["image_bytes"], deadline)
            ack = ctypes.create_string_buffer(b"K", 1)
            self._io(handle, ack, write=True, deadline=deadline)
            validate_metadata(meta, k.GetTickCount64(), self.pid, allow_selftest=self.allow_selftest)
            with Image.open(io.BytesIO(payload)) as source:
                if source.format != "PPM" or source.size != (meta["width"], meta["height"]):
                    raise ValueError("Image does not match frame metadata")
                image = source.convert("RGB")
            validate_metadata(meta, k.GetTickCount64(), self.pid, allow_selftest=self.allow_selftest)
            meta["age_ms_at_read"] = k.GetTickCount64() - meta["received_tick_ms"]
            meta["read_completed_unix"] = time.time()
            meta["timestamp_basis"] = "receiver_packet_receipt_and_decode_not_phone_render_time"
            return image, meta
        finally:
            k.CloseHandle(handle)

    def save(self, destination: Path):
        image, meta = self.grab()
        destination.parent.mkdir(parents=True, exist_ok=True)
        image.save(destination, format="PNG")
        destination.with_suffix(".json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
        return meta
