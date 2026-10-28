// SPDX-License-Identifier: GPL-2.0-only
#include <iostream>
#include <thread>
#include "mt7922_device.h"
#include "secure_pipe.h"
namespace {
void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
}
int main() {
  try {
    mt7922::PipeSecurity security;PACL dacl=nullptr;BOOL present=FALSE,defaulted=TRUE;
    check(GetSecurityDescriptorDacl(security.attributes()->lpSecurityDescriptor,&present,&dacl,&defaulted) &&
          present && dacl && !defaulted && dacl->AceCount==3,"explicit three-principal DACL required");
    SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;
    check(GetSecurityDescriptorControl(security.attributes()->lpSecurityDescriptor,&control,&revision) && (control&SE_DACL_PROTECTED),
          "DACL must not inherit broad permissions");
    for(DWORD i=0;i<dacl->AceCount;++i) {
      void* raw=nullptr;check(GetAce(dacl,i,&raw)!=0,"read ACE");const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
      check(ace->Header.AceType==ACCESS_ALLOWED_ACE_TYPE && ace->Mask==GENERIC_ALL,"unexpected ACE shape");
      auto sid=const_cast<DWORD*>(&ace->SidStart);
      check(!IsWellKnownSid(sid,WinWorldSid) && !IsWellKnownSid(sid,WinBuiltinUsersSid) &&
            !IsWellKnownSid(sid,WinAuthenticatedUserSid),"broad pipe principal");
    }
    const auto until=GetTickCount64()+5000;
    const std::string name="\\\\.\\pipe\\mt7922-offline-test-"+std::to_string(GetCurrentProcessId());
    SecurePipeServer server(name,[until]{return GetTickCount64()>=until;},security.attributes());
    check(server.valid(),"private test pipe creation");
    SecurePipeServer duplicate(name,[]{return true;},security.attributes());check(!duplicate.valid(),"first-instance protection");
    SecurePipeServer no_acl(name+"-no-acl",[]{return true;},nullptr);check(!no_acl.valid(),"missing ACL rejected");
    std::atomic<bool> client_ok{false};
    std::thread client([&] {
      HANDLE h=CreateFileA(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
      if(h==INVALID_HANDLE_VALUE)return;
      DWORD done=0;const std::array<uint8_t,6> payload={2,1,2,3,4,5};
      const radio_link_header header{RADIO_LINK_SET_ADDRESS,0,6};
      bool ok=WriteFile(h,&header,3,&done,nullptr)!=0;
      ok=ok && WriteFile(h,reinterpret_cast<const uint8_t*>(&header)+3,5,&done,nullptr);
      ok=ok && WriteFile(h,payload.data(),6,&done,nullptr);
      // Bounded synchronous reads prevent a broken server from hanging tests.
      const auto deadline=GetTickCount64()+3000;DWORD available=0;
      while(ok && GetTickCount64()<deadline) {
        if(!PeekNamedPipe(h,nullptr,0,nullptr,&available,nullptr)){ok=false;break;}
        if(available>=16)break;Sleep(1);
      }
      std::array<uint8_t,16> received{};
      ok=ok && available>=16 && ReadFile(h,received.data(),16,&done,nullptr) && done==16 &&
         mt7922::le16(received,0)==1 && mt7922::le32(received,4)==8 && mt7922::le32(received,8)==3;
      client_ok=ok;CloseHandle(h);
    });
    bool accepted=server.wait_for_client();radio_link_header header{};std::array<uint8_t,6> payload{};
    bool received=accepted && server.read_exact(&header,8) && server.read_exact(payload.data(),6);
    const radio_link_hello hello{3,6,20,{0,0}};
    bool sent=received && server.send(RADIO_LINK_HELLO,{reinterpret_cast<const uint8_t*>(&hello),8},500);
    client.join();check(received && sent && client_ok && header.type==6 && header.length==6 && payload[0]==2,"fragmented duplex byte stream");
    check(!server.read_exact(&header,8),"disconnect detected");server.drop_client();
    const auto before=GetTickCount64();
    SecurePipeServer canceled(name+"-cancel",[]{return true;},security.attributes());
    check(canceled.valid() && !canceled.wait_for_client() && GetTickCount64()-before<2000,"cancel pending accept");
    std::cout<<"PASS private ACL, first-instance, fragmented byte stream, disconnect and cancellation; no device access\n";return 0;
  } catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
