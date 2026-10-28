// SPDX-License-Identifier: GPL-2.0-only
// Actual native OWL process -> private pipe -> production bridge/adapter ->
// in-memory device model. -n prevents host adapter creation; no driver is opened.
#include <iostream>
#include "mt7922_device.h"
#include "serve.h"
namespace {
struct Model {
  std::array<uint8_t,6> mac{2,0,0,0,0,1};
  uint32_t channel=6,addresses=0,frames=0,polls=0;bool stopped=false;
  std::array<uint32_t,8> rates{};
  mt7922::Bytes status() {
    mt7922::Bytes p(256);mt7922::put32(p,0,0x3253524d);mt7922::put32(p,4,1);mt7922::put32(p,8,256);
    mt7922::put32(p,12,1|2|4|16|32|64|512|1024);mt7922::put32(p,16,12);
    mt7922::put32(p,36,channel);mt7922::put32(p,40,channel);mt7922::put32(p,44,20);mt7922::put32(p,72,1);
    std::copy(mac.begin(),mac.end(),p.begin()+50);return p;
  }
  mt7922::IoResult exchange(uint32_t code,std::span<const uint8_t> p,size_t capacity) {
    using namespace mt7922;
    if(code==Receive){require(capacity==16392 && p.empty(),"wrong RX request");return {259,{}};}
    require(capacity==256,"wrong output capacity");auto out=status();
    if(code==Start)validate_start(p);
    else if(code==Address) {
      require(p.size()==6 && !(p[0]&1),"invalid OWL source address");
      std::copy(p.begin(),p.end(),mac.begin());++addresses;out=status();
    } else if(code==Transmit) {
      require(addresses && p.size()>=36 && le32(p,8)==p.size()-12 && le32(p,0)<=1,"invalid OWL TX envelope");
      require(std::equal(mac.begin(),mac.end(),p.begin()+22),"OWL TX source was not programmed");
      require((p[12]&3)==0 && (p[13]&3)==0,"unexpected 802.11 form");
      const std::array<uint32_t,8> values={12,18,24,36,48,72,96,108};
      const auto found=std::find(values.begin(),values.end(),le32(p,4));
      require(found!=values.end(),"unknown OWL TX rate");++rates[size_t(found-values.begin())];++frames;
      put32(out,208,frames%256);put32(out,212,1);
    } else if(code==Channel) {
      require(p.size()==12 && le32(p,0)==le32(p,4) && le32(p,8)==20,"unexpected OWL channel geometry");
      channel=le32(p,0);out=status();
    } else if(code==Service){require(p.empty(),"unexpected poll input");++polls;}
    else throw std::runtime_error("unexpected model IOCTL");
    return {0,std::move(out)};
  }
};
struct Process {
  PROCESS_INFORMATION value{};
  ~Process() {
    if(value.hProcess) {
      if(WaitForSingleObject(value.hProcess,0)==WAIT_TIMEOUT) {
        TerminateProcess(value.hProcess,99);WaitForSingleObject(value.hProcess,5000);
      }
      CloseHandle(value.hThread);CloseHandle(value.hProcess);
    }
  }
};
}
int wmain(int argc,wchar_t** argv) {
  try {
    mt7922::require(argc==2,"supply the built native awdld.exe path");
    const std::filesystem::path exe=std::filesystem::absolute(argv[1]);
    mt7922::require(std::filesystem::is_regular_file(exe) && exe.wstring().find(L'"')==std::wstring::npos,"invalid executable path");
    std::atomic<bool> stopped{false};const auto deadline=GetTickCount64()+3500;
    auto stopping=[&]{if(GetTickCount64()>=deadline)stopped=true;return stopped.load();};
    mt7922::PipeSecurity security;SecurePipeServer pipe(RADIO_LINK_PIPE_NAME,stopping,security.attributes());
    mt7922::require(pipe.valid(),"test pipe unavailable; existing backend left untouched");
    Model model;mt7922::Adapter adapter(model);mt7925::Bridge session(adapter,6,stopping,std::chrono::milliseconds(100),true);
    mt7922::Config config;config.country={'U','S'};config.mac=model.mac;config.primary=config.center=6;
    const mt7922::Bytes placeholder={1};session.start(mt7922::start_request(placeholder,placeholder,config));
    Process child;STARTUPINFOW startup{};startup.cb=sizeof(startup);
    std::wstring command=L"\""+exe.wstring()+L"\" -c 6 -n -r";
    if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,exe.parent_path().c_str(),&startup,&child.value))
      throw mt7925::win_error("launch device-free AWDL test child");
    try {mt7922::serve(pipe,session,6,stopped);}catch(const mt7925::StopRequested&){}
    stopped=true;pipe.drop_client();
    mt7922::require(WaitForSingleObject(child.value.hProcess,5000)==WAIT_OBJECT_0,"AWDL did not exit after pipe shutdown");
    DWORD exit_code=99;mt7922::require(GetExitCodeProcess(child.value.hProcess,&exit_code)!=0 && !exit_code,"AWDL child failed");
    mt7922::require(model.addresses==1 && model.frames>=2 && model.polls>0 && model.channel==6,"actual AWDL command sequence incomplete");
    std::cout<<"{\"actual_awdl_process\":true,\"programmed_addresses\":"<<model.addresses<<",\"accepted_synthetic_tx\":"<<model.frames
      <<",\"service_calls\":"<<model.polls<<",\"rate_12_mbps_frames\":"<<model.rates[2]
      <<",\"child_exit_code\":"<<exit_code<<",\"hardware_access\":false,\"host_adapter_created\":false,\"rf_started\":false}\n";
    return 0;
  } catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
