@echo off
cd /d "%~dp0"
if exist "%~dp0AirWindowsNative.exe" (
    start "" "%~dp0AirWindowsNative.exe" --frames
) else (
    start "" "%~dp0AirWindows.exe" --frames
)
