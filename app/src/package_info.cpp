// Documentation this follows (Microsoft Learn, read 2026-11-13):
//   https://learn.microsoft.com/windows/msix/desktop/desktop-to-uwp-behind-the-scenes
//     New files and folders a packaged app creates under AppData go to a private
//     per-user, per-package location; files that already exist there are opened in place.
//   https://learn.microsoft.com/uwp/api/windows.storage.applicationdata.localfolder
//     LocalFolder: the root folder of the local app data store.
//   https://learn.microsoft.com/windows/apps/develop/data/store-and-retrieve-app-data
//     ApplicationData is for packaged apps; unpackaged apps have no app data store.

#include "package_info.h"

#include <windows.h>
#include <appmodel.h>
#include <sddl.h>
#include <userenv.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>

#include <cwchar>
#include <exception>
#include <thread>

namespace package_info {

bool HasIdentity() {
    UINT32 length = 0;
    return GetCurrentPackageFamilyName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
}

std::wstring FamilyName() {
    UINT32 length = 0;
    if (GetCurrentPackageFamilyName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        return {};
    }
    std::wstring name(length, L'\0');
    if (GetCurrentPackageFamilyName(&length, name.data()) != ERROR_SUCCESS) {
        return {};
    }
    name.resize(wcslen(name.c_str()));
    return name;
}

// https://learn.microsoft.com/windows/win32/api/userenv/nf-userenv-deriveappcontainersidfromappcontainername
//   "Gets the SID of the specified profile"; freed with FreeSid. The profile name passed is
//   the package family name. Measured 2026-11-13 with a registered package: the manifest's
//   firewall rules name the executable's path in the install folder and carry no package
//   SID, so network_check.cpp matches by path first; the SID match is only a fallback.
std::wstring Sid() {
    const std::wstring family = FamilyName();
    if (family.empty()) {
        return {};
    }
    PSID sid = nullptr;
    if (FAILED(DeriveAppContainerSidFromAppContainerName(family.c_str(), &sid)) || !sid) {
        return {};
    }
    std::wstring text;
    LPWSTR converted = nullptr;
    if (ConvertSidToStringSidW(sid, &converted) && converted) {
        text = converted;
        LocalFree(converted);
    }
    FreeSid(sid);
    return text;
}

std::wstring LocalFolder(std::wstring& error) {
    std::wstring path;
    error.clear();
    if (!HasIdentity()) {
        error = L"no package identity";
        return path;
    }
    // A thread of its own, in the multi-threaded apartment: the caller may run before
    // COM is set up on its thread, or in another apartment.
    std::thread worker([&path, &error] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (const winrt::hresult_error& failure) {
            wchar_t code[32];
            swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(static_cast<int32_t>(failure.code())));
            error = std::wstring(L"COM could not start (") + code + L")";
            return;
        }
        try {
            path = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str();
        } catch (const winrt::hresult_error& failure) {
            wchar_t code[32];
            swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(static_cast<int32_t>(failure.code())));
            error = std::wstring(L"ApplicationData.LocalFolder failed (") + code + L": " + failure.message().c_str() + L")";
            path.clear();
        } catch (const std::exception& failure) {
            const std::string what = failure.what();
            error = L"ApplicationData.LocalFolder failed (" + std::wstring(what.begin(), what.end()) + L")";
            path.clear();
        }
        winrt::uninit_apartment();
    });
    worker.join();
    return path;
}

}  // namespace package_info
