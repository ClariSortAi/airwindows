// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include "mt7925_device.h" // Pinned bounded file loader / SHA-256 helper only.
#include "mt7922_adapter.h"
#include <sddl.h>

namespace mt7922 {
inline Bytes firmware_request(const std::filesystem::path& folder,const Config& c) {
  const auto patch=mt7925::pinned_image(folder/L"WIFI_MT7922_patch_mcu_1_1_hdr.bin",
      "7fc9075d6d54b31e0539734614ca971fa52caf41b642a3000c8a4501591a4d98");
  const auto ram=mt7925::pinned_image(folder/L"WIFI_RAM_CODE_MT7922_1.bin",
      "d100e5e04c324fc8da6302112d1e0b4fac952286447d2419eabcd8fb0a1f5b93");
  return start_request(patch,ram,c);
}
class WindowsTransport {
  HANDLE handle_=INVALID_HANDLE_VALUE;
public:
  WindowsTransport() {
    handle_=CreateFileW(L"\\\\.\\AirWindowsMt7922Bringup",GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(handle_==INVALID_HANDLE_VALUE)throw mt7925::win_error("open installed MT7922 candidate (no installation attempted)");
  }
  ~WindowsTransport(){if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);}
  WindowsTransport(const WindowsTransport&)=delete;WindowsTransport& operator=(const WindowsTransport&)=delete;
  IoResult exchange(uint32_t code,std::span<const uint8_t> input,size_t capacity) {
    require(input.size()<=64+2*8*1024*1024 && capacity<=ReceiveBytes,"oversize device request");
    IoResult result;result.bytes.resize(capacity);DWORD count=0;
    if(!DeviceIoControl(handle_,code,input.empty()?nullptr:const_cast<uint8_t*>(input.data()),DWORD(input.size()),
        result.bytes.data(),DWORD(capacity),&count,nullptr)) {
      result.error=GetLastError();result.bytes.clear();return result;
    }
    require(count<=capacity,"driver returned excess bytes");result.bytes.resize(count);return result;
  }
};
// Explicit pipe ACL: this Windows user, administrators and SYSTEM. The bridge
// can perform privileged RF operations, so it must not inherit a broad DACL.
class PipeSecurity {
  PSECURITY_DESCRIPTOR descriptor_=nullptr;
  SECURITY_ATTRIBUTES attributes_{};
public:
  PipeSecurity() {
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))throw mt7925::win_error("open process token");
    struct TokenCleanup{HANDLE h;~TokenCleanup(){CloseHandle(h);}} cleanup{token};
    DWORD bytes=0;GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
    require(bytes>=sizeof(TOKEN_USER) && bytes<=65536,"invalid token user size");
    std::vector<uint64_t> buffer((bytes+7u)/8u);
    if(!GetTokenInformation(token,TokenUser,buffer.data(),bytes,&bytes))throw mt7925::win_error("read process user");
    LPWSTR sid=nullptr;
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&sid))throw mt7925::win_error("encode process user");
    struct LocalCleanup{HLOCAL p;~LocalCleanup(){LocalFree(p);}} free_sid{sid};
    const std::wstring sddl=L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;"+std::wstring(sid)+L")";
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor_,nullptr))
      throw mt7925::win_error("build private radio pipe ACL");
    attributes_.nLength=sizeof(attributes_);attributes_.lpSecurityDescriptor=descriptor_;attributes_.bInheritHandle=FALSE;
  }
  ~PipeSecurity(){if(descriptor_)LocalFree(descriptor_);}
  PipeSecurity(const PipeSecurity&)=delete;PipeSecurity& operator=(const PipeSecurity&)=delete;
  SECURITY_ATTRIBUTES* attributes(){return &attributes_;}
};
} // namespace mt7922
