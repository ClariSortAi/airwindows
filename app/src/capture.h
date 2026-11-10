// Saving what is on the phone: where captures go, how they are named, and
// writing one picture as a PNG file or onto the clipboard.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace capture {

// Pictures\AirWindows and Videos\AirWindows, or `overrideDirectory` for both
// when it is set (--capture-dir). Created on demand; empty if that fails.
std::wstring PicturesFolder(const std::wstring& overrideDirectory);
std::wstring VideosFolder(const std::wstring& overrideDirectory);

// "AirWindows 2026-11-10 10.15.30", from the local clock.
std::wstring TimestampName();

// <folder>\<name><extension>, or "<name> (2)<extension>" and so on if taken.
std::wstring FreePath(const std::wstring& folder, const std::wstring& name, const wchar_t* extension);

// `rgb` is width*height*3 bytes, top row first. On failure `error` says why.
bool SavePng(const std::wstring& path, const std::vector<uint8_t>& rgb, int width, int height, std::wstring& error);
bool CopyToClipboard(HWND owner, const std::vector<uint8_t>& rgb, int width, int height);

}  // namespace capture
