// SPDX-License-Identifier: GPL-2.0-only
#include <iostream>
#include <deque>
#include "mt7922_device.h"
#include "mt7925_bridge.h"
extern "C" {
#include "../driver/mt7922/radio_abi.h"
}
using namespace mt7922;
static_assert(Start==MT2_IOCTL_RADIO_START && Service==MT2_IOCTL_RADIO_SERVICE && Receive==MT2_IOCTL_RADIO_RECEIVE);
static_assert(Transmit==MT2_IOCTL_RADIO_TRANSMIT && Channel==MT2_IOCTL_RADIO_CHANNEL && Status==MT2_IOCTL_RADIO_STATUS);
static_assert(Address==MT2_IOCTL_RADIO_ADDRESS && Stop==MT2_IOCTL_STOP && StatusBytes==MT2_RADIO_STATUS_BYTES);
namespace {
int checks=0;
void check(bool b,const char* reason){++checks;if(!b)throw std::runtime_error(reason);}
template<class F> void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,"expected rejection");}
const std::array<uint8_t,6> mac={2,3,4,5,6,7};
Bytes status(int32_t result=0) {
  Bytes p(256);put32(p,0,0x3253524d);put32(p,4,1);put32(p,8,256);
  put32(p,12,1|2|4|16|32|64|512|1024);put32(p,16,12);put32(p,20,uint32_t(result));
  put32(p,28,result?0x80000011:0);put32(p,36,6);put32(p,40,6);put32(p,44,20);put32(p,72,1);
  std::copy(mac.begin(),mac.end(),p.begin()+50);return p;
}
Bytes stop_status(int32_t prior,int32_t error=0,bool quiet=true) {
  Bytes p(128);put32(p,0,0x3253544d);put32(p,4,1);put32(p,8,128);put32(p,12,8|64|(quiet?128:0));
  put32(p,20,uint32_t(prior));put32(p,24,uint32_t(error));return p;
}
Bytes frame() {
  Bytes p(24);p[0]=0xd0;p[4]=0xff;std::copy(mac.begin(),mac.end(),p.begin()+10);
  std::fill(p.begin()+16,p.begin()+22,uint8_t(0x55));return p;
}
Bytes radiotap(uint8_t rate=24) {
  Bytes p(9);p[2]=9;p[4]=4;p[8]=rate;const auto f=frame();p.insert(p.end(),f.begin(),f.end());return p;
}
Bytes record() {
  const auto rt=radiotap();Bytes p(8);p[0]=2;put32(p,4,uint32_t(rt.size()));p.insert(p.end(),rt.begin(),rt.end());return p;
}
struct Expected {uint32_t code;Bytes input;size_t capacity;IoResult output;};
struct Model {
  std::deque<Expected> expected;
  unsigned calls=0;
  IoResult exchange(uint32_t code,std::span<const uint8_t> input,size_t capacity) {
    ++calls;
    check(!expected.empty(),"unexpected device call");auto e=std::move(expected.front());expected.pop_front();
    check(code==e.code && capacity==e.capacity,"wrong IOCTL or output capacity");
    check(Bytes(input.begin(),input.end())==e.input,"incorrect translated request");return e.output;
  }
  void add(uint32_t code,Bytes input,Bytes output,size_t capacity=256,uint32_t error=0) {
    expected.push_back({code,std::move(input),capacity,{error,std::move(output)}});
  }
};
void test_start() {
  Config c;c.country={'U','S'};c.mac=mac;c.primary=c.center=6;c.power=-12;c.fcs=1;
  const Bytes patch={11,22},ram={33,44,55};const auto request=start_request(patch,ram,c);
  check(request.size()==69 && le32(request,52)==0xfffffff4u && le32(request,56)==1 &&
        Bytes(request.begin()+64,request.end())==Bytes({11,22,33,44,55}),"start concatenation and signed cap");
  Model m;m.add(Start,request,status());Adapter d(m);mt7925::Bridge session(d,6,[]{return false;});
  check(session.start(request).ready && m.expected.empty(),"bridge start through real adapter");
  for(size_t i=0;i<request.size();++i)rejects([&]{d.call(mt7925::Start,std::span(request).first(i));});
  for(size_t off:{0u,4u,8u,12u,16u,18u,24u,28u,32u,36u,40u,44u,48u,56u,60u}) {
    auto bad=request;bad[off]=0xff;rejects([&]{d.call(mt7925::Start,bad);});
  }
  auto bad=request;bad.push_back(0);rejects([&]{d.call(mt7925::Start,bad);});
  bad=request;put32(bad,52,45);rejects([&]{d.call(mt7925::Start,bad);});
  bad=request;put32(bad,52,0xffffff80);rejects([&]{d.call(mt7925::Start,bad);});
  check(m.expected.empty() && m.calls==1,"invalid start never calls transport");
}
void test_status() {
  auto p=status();check(decode_status(p,mt7925::Status).ready,"healthy status");
  for(size_t n=0;n<p.size();++n)rejects([&]{decode_status(std::span(p).first(n),mt7925::Status);});
  for(uint32_t flag:{1u,2u,32u,64u,512u,1024u}) {
    auto bad=p;put32(bad,12,le32(bad,12)&~flag);rejects([&]{decode_status(bad,mt7925::Status);});
  }
  for(uint32_t flag:{8u,128u,256u,2048u}) {
    auto bad=p;put32(bad,12,le32(bad,12)|flag);rejects([&]{decode_status(bad,mt7925::Status);});
  }
  for(size_t i=216;i<256;++i){auto bad=p;bad[i]=1;rejects([&]{decode_status(bad,mt7925::Status);});}
  for(int32_t result:{-1,-2,-3}) {
    auto r=decode_status(status(result),mt7925::Channel);check(r.ready && !r.error && r.result==result,"recoverable rejection");
  }
  auto bad=p;put32(bad,12,1);put32(bad,28,0xc0000428);
  check(decode_status(bad,mt7925::Start).error!=0,"pre-core NTSTATUS failure surfaced");
  check(decode_stop(stop_status(-99)).stopped,"prior failure does not obscure successful stop");
  check(!decode_stop(stop_status(0,-9)).stopped && !decode_stop(stop_status(0,0,false)).stopped,"failed/unquiesced stop rejected");
  auto accepted=p;put32(accepted,208,4);put32(accepted,212,1);
  check(decode_status(accepted,mt7925::Tx).token==4,"TX token accepted");
  rejects([&]{decode_status(accepted,mt7925::Status);});rejects([&]{decode_status(p,mt7925::Tx);});
  put32(accepted,208,256);rejects([&]{decode_status(accepted,mt7925::Tx);});
}
void test_bridge() {
  Model m;Adapter d(m);mt7925::Bridge session(d,6,[]{return false;},std::chrono::milliseconds(50));
  const auto rt=radiotap();const radio_link_header tx{RADIO_LINK_TX,0,uint32_t(rt.size())};
  rejects([&]{session.command(tx,rt);}); // Identity must reach firmware first.
  m.add(Address,Bytes(mac.begin(),mac.end()),status(-3));m.add(Address,Bytes(mac.begin(),mac.end()),status());
  session.command({RADIO_LINK_SET_ADDRESS,0,6},mac);
  for(uint32_t rate:{12u,18u,24u,36u,48u,72u,96u,108u}) {
    const auto air=radiotap(uint8_t(rate));Bytes request(12);put32(request,0,1);put32(request,4,rate);put32(request,8,24);
    const auto f=frame();request.insert(request.end(),f.begin(),f.end());auto reply=status();put32(reply,208,25);put32(reply,212,1);
    // Both temporary ownership barriers are retried; there is no replay after
    // a Windows error or malformed/uncertain successful response.
    m.add(Transmit,request,status(-3));m.add(Transmit,request,status(-2));m.add(Transmit,request,reply);
    session.command({RADIO_LINK_TX,0,uint32_t(air.size())},air);
  }
  auto wrong=rt;wrong[19]^=2;rejects([&]{session.command(tx,wrong);});
  Bytes channel(12);put32(channel,0,44);put32(channel,4,44);put32(channel,8,20);
  auto reply=status();put32(reply,36,44);put32(reply,40,44);
  m.add(Channel,channel,status(-3));m.add(Channel,channel,reply);const std::array<uint8_t,1> ch={44};
  session.command({RADIO_LINK_SET_CHANNEL,0,1},ch);
  m.add(Service,{},reply);m.add(Receive,{},record(),ReceiveBytes);
  check(session.poll()==rt,"complete receive record reaches bridge client");
  m.add(Service,{},reply);m.add(Receive,{},{},ReceiveBytes,259);check(session.poll().empty(),"empty queue is normal");
  m.add(Service,{},reply);auto broken=record();broken[4]++;m.add(Receive,{},broken,ReceiveBytes);
  rejects([&]{session.poll();});
  m.add(Service,{},Bytes{},256,5);rejects([&]{session.poll();});
  auto fault=status(-9);put32(fault,12,1|2|8|512|1024);m.add(Service,{},fault);
  rejects([&]{session.poll();});check(m.expected.empty(),"fatal service does not dequeue");
  m.add(Stop,{},stop_status(-9),128);check(session.stop().stopped,"bridge explicit shutdown");
  check(m.expected.empty(),"all expected hardware-free operations consumed");
  rejects([&]{d.call(0x999);});rejects([&]{d.call(mt7925::Poll,Bytes{1});});
}
}
int main() {
  try{test_start();test_status();test_bridge();std::cout<<"PASS "<<checks<<" checks; mock device only, no RF\n";return 0;}
  catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
