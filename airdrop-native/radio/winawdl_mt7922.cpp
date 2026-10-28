// SPDX-License-Identifier: GPL-2.0-only
// Native Windows MT7922 adapter for the pinned AWDL radio-link v3 bridge.
#include <iostream>
#include <set>
#include "mt7922_device.h"
#include "serve.h"

namespace {
std::atomic<bool> stop_requested{false};
BOOL WINAPI interrupted(DWORD event) {
  if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT) {stop_requested=true;return TRUE;}
  return FALSE;
}
int number(const std::wstring& s) {
  size_t end=0;const auto n=std::stoi(s,&end);
  mt7922::require(end==s.size(),"invalid numeric argument");return n;
}
}
int wmain(int argc,wchar_t** argv) {
  if(argc==1 || (argc==2 && std::wstring(argv[1])==L"--help")) {
    std::cout<<"Experimental native Windows MT7922 radio-link v3 bridge; hardware unqualified.\n"
      "--run --firmware DIR --country XX --channel N --power-half-dbm N --fcs 0|1 --seconds 1..600\n"
      "Optional: --environment 0|1|2 --power-type 0..4 --fixed-channel --stop-file ABSOLUTE_PATH\n"
      "All RF configuration is explicit. FCS must match measured receive behavior.\n"
      "--run requires an already installed candidate driver; never installs, elevates or changes boot policy.\n"
      "Local queue acceptance does not prove peer receipt or AirDrop visibility.\n";
    return 0;
  }
  try {
    mt7922::Config c;std::filesystem::path folder,stop_file;std::set<std::wstring> seen;
    bool run=false,fixed=false;int seconds=0;
    for(int i=1;i<argc;++i) {
      const std::wstring key=argv[i];mt7922::require(seen.insert(key).second,"duplicate option");
      if(key==L"--run"){run=true;continue;}
      if(key==L"--fixed-channel"){fixed=true;continue;}
      mt7922::require(i+1<argc,"missing argument value");const std::wstring value=argv[++i];
      if(key==L"--firmware")folder=value;
      else if(key==L"--stop-file")stop_file=value;
      else if(key==L"--country") {
        mt7922::require(value.size()==2 && value[0]>='A' && value[0]<='Z' && value[1]>='A' && value[1]<='Z',
                        "country must be the actual uppercase alpha2 country, not a default");
        c.country={uint8_t(value[0]),uint8_t(value[1])};
      } else if(key==L"--channel")c.primary=c.center=uint32_t(number(value));
      else if(key==L"--power-half-dbm")c.power=number(value);
      else if(key==L"--fcs")c.fcs=uint32_t(number(value));
      else if(key==L"--environment")c.environment=uint32_t(number(value));
      else if(key==L"--power-type")c.power_type=uint32_t(number(value));
      else if(key==L"--seconds")seconds=number(value);
      else throw std::runtime_error("unknown option");
    }
    for(const auto* required:{L"--run",L"--firmware",L"--country",L"--channel",L"--power-half-dbm",L"--fcs",L"--seconds"})
      mt7922::require(seen.count(required)!=0,"missing explicit required configuration (see --help)");
    mt7922::require(run && !folder.empty() && seconds>=1 && seconds<=600 && c.primary<=165,"invalid bounded trial configuration");
    if(!stop_file.empty())mt7922::require(stop_file.is_absolute() && !mt7925::stop_file_present(stop_file),
                                         "stop file must be an absent absolute path");
    if(BCryptGenRandom(nullptr,c.mac.data(),DWORD(c.mac.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)
      throw std::runtime_error("initial local MAC generation failed");
    c.mac[0]=uint8_t((c.mac[0]|2u)&0xfeu);
    const auto request=mt7922::firmware_request(folder,c);
    const auto deadline=GetTickCount64()+uint64_t(seconds)*1000;
    auto stopping=[deadline,stop_file] {
      if(GetTickCount64()>=deadline || mt7925::stop_file_present(stop_file))stop_requested=true;
      return stop_requested.load();
    };
    mt7922::PipeSecurity security;
    SecurePipeServer pipe(RADIO_LINK_PIPE_NAME,stopping,security.attributes());
    mt7922::require(pipe.valid(),"private radio pipe unavailable (another backend may be running)");
    if(!SetConsoleCtrlHandler(interrupted,TRUE))throw mt7925::win_error("register stop handler");
    mt7922::WindowsTransport transport;mt7922::Adapter driver(transport);
    mt7925::Bridge session(driver,c.primary,stopping,std::chrono::milliseconds(2000),fixed);
    std::exception_ptr failure;
    try {
      session.start(request);
      std::cout<<"Local radio sequence completed; waiting for AWDL client. iOS reception remains unverified."<<std::endl;
      mt7922::serve(pipe,session,uint8_t(c.primary),stop_requested);
    } catch(const mt7925::StopRequested&) {
      std::cout<<"Radio stop requested or trial limit reached.\n";
    } catch(...) {failure=std::current_exception();}
    stop_requested=true;
    // All worker access has joined before explicit STOP. Handle cleanup is the
    // fallback, not evidence that the chip actually quiesced.
    bool stopped=false;
    try {
      const auto r=session.stop();stopped=r.stopped && !r.result && !r.stop_error && (!r.published || r.quiescent);
      std::cerr<<"STOP result="<<r.result<<" stop_error="<<r.stop_error<<" published="<<r.published<<" quiescent="<<r.quiescent<<'\n';
    } catch(const std::exception& e){std::cerr<<"Explicit STOP failed: "<<e.what()<<'\n';}
    std::cerr<<"Receive counters: ";session.rx_diagnostics().write_json(std::cerr);std::cerr<<'\n';
    if(!stopped)std::cerr<<"Hardware shutdown was not confirmed; driver retains DMA storage.\n";
    if(failure)std::rethrow_exception(failure);
    return stopped?0:1;
  } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
