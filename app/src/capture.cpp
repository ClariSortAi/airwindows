#include "capture.h"

#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace capture {
namespace {

std::wstring Folder(REFKNOWNFOLDERID known, const std::wstring& overrideDirectory) {
    std::wstring folder = overrideDirectory;
    if (folder.empty()) {
        PWSTR base = nullptr;
        if (FAILED(SHGetKnownFolderPath(known, KF_FLAG_CREATE, nullptr, &base))) {
            LOGE("[CAPTURE] could not locate the user's media folder");
            return {};
        }
        folder = std::wstring(base) + L"\\AirWindows";
        CoTaskMemFree(base);
    }
    const int result = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS && result != ERROR_FILE_EXISTS) {
        LOGE("[CAPTURE] could not create %ls (error %d)", folder.c_str(), result);
        return {};
    }
    return folder;
}

std::wstring Describe(const wchar_t* step, HRESULT hr) {
    wchar_t text[160];
    swprintf_s(text, L"%ls failed (0x%08lX)", step, static_cast<unsigned long>(hr));
    return text;
}

}  // namespace

std::wstring PicturesFolder(const std::wstring& overrideDirectory) {
    return Folder(FOLDERID_Pictures, overrideDirectory);
}

std::wstring VideosFolder(const std::wstring& overrideDirectory) {
    return Folder(FOLDERID_Videos, overrideDirectory);
}

std::wstring TimestampName() {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t name[64];
    swprintf_s(name, L"AirWindows %04d-%02d-%02d %02d.%02d.%02d", now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute, now.wSecond);
    return name;
}

std::wstring FreePath(const std::wstring& folder, const std::wstring& name, const wchar_t* extension) {
    std::wstring path = folder + L"\\" + name + extension;
    for (int n = 2; n < 1000 && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) {
        path = folder + L"\\" + name + L" (" + std::to_wstring(n) + L")" + extension;
    }
    return path;
}

bool SavePng(const std::wstring& path, const std::vector<uint8_t>& rgb, int width, int height, std::wstring& error) {
    if (width <= 0 || height <= 0 || rgb.size() < static_cast<size_t>(width) * height * 3) {
        error = L"The picture is empty.";
        return false;
    }
    // The encoder takes BGR rows padded to four bytes.
    const UINT stride = (static_cast<UINT>(width) * 3 + 3) & ~3u;
    std::vector<uint8_t> bgr(static_cast<size_t>(stride) * height);
    for (int y = 0; y < height; ++y) {
        const uint8_t* source = rgb.data() + static_cast<size_t>(y) * width * 3;
        uint8_t* target = bgr.data() + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            target[x * 3] = source[x * 3 + 2];
            target[x * 3 + 1] = source[x * 3 + 1];
            target[x * 3 + 2] = source[x * 3];
        }
    }

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    const wchar_t* step = L"Starting the image encoder";
    if (SUCCEEDED(hr)) { step = L"Creating the file"; hr = factory->CreateStream(&stream); }
    if (SUCCEEDED(hr)) { hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE); }
    if (SUCCEEDED(hr)) { step = L"Opening the PNG encoder"; hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder); }
    if (SUCCEEDED(hr)) { hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache); }
    if (SUCCEEDED(hr)) { hr = encoder->CreateNewFrame(&frame, nullptr); }
    if (SUCCEEDED(hr)) { hr = frame->Initialize(nullptr); }
    if (SUCCEEDED(hr)) { hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)); }
    if (SUCCEEDED(hr)) { hr = frame->SetPixelFormat(&format); }
    if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) { hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT; }
    if (SUCCEEDED(hr)) { step = L"Writing the picture"; hr = frame->WritePixels(static_cast<UINT>(height), stride, static_cast<UINT>(bgr.size()), bgr.data()); }
    if (SUCCEEDED(hr)) { hr = frame->Commit(); }
    if (SUCCEEDED(hr)) { hr = encoder->Commit(); }
    if (FAILED(hr)) {
        error = Describe(step, hr);
        frame.Reset();
        encoder.Reset();
        stream.Reset();
        // Do not leave a partial file where a screenshot was expected.
        if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            LOGW("[CAPTURE] could not remove the partial file %ls (error %lu)", path.c_str(), GetLastError());
        }
        return false;
    }
    return true;
}

bool CopyToClipboard(HWND owner, const std::vector<uint8_t>& rgb, int width, int height) {
    if (width <= 0 || height <= 0 || rgb.size() < static_cast<size_t>(width) * height * 3) {
        return false;
    }
    const size_t stride = (static_cast<size_t>(width) * 3 + 3) & ~static_cast<size_t>(3);
    const size_t bytes = sizeof(BITMAPINFOHEADER) + stride * height;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        LOGW("[CAPTURE] no memory for the clipboard copy (%zu bytes)", bytes);
        return false;
    }
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory));
    if (!header) {
        GlobalFree(memory);
        return false;
    }
    memset(header, 0, sizeof(*header));
    header->biSize = sizeof(*header);
    header->biWidth = width;
    header->biHeight = height;  // bottom-up, as the clipboard format expects
    header->biPlanes = 1;
    header->biBitCount = 24;
    header->biCompression = BI_RGB;
    auto* pixels = reinterpret_cast<uint8_t*>(header + 1);
    for (int y = 0; y < height; ++y) {
        const uint8_t* source = rgb.data() + static_cast<size_t>(height - 1 - y) * width * 3;
        uint8_t* target = pixels + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            target[x * 3] = source[x * 3 + 2];
            target[x * 3 + 1] = source[x * 3 + 1];
            target[x * 3 + 2] = source[x * 3];
        }
    }
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) {
        LOGW("[CAPTURE] the clipboard is busy (error %lu); the screenshot was still saved", GetLastError());
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    const bool placed = SetClipboardData(CF_DIB, memory) != nullptr;
    CloseClipboard();
    if (!placed) {
        GlobalFree(memory);  // the clipboard owns it only on success
    }
    return placed;
}

}  // namespace capture
