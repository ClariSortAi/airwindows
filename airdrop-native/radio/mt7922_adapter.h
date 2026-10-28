// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include "mt7925_codec.h"
#include <limits>

// Translate the pinned AWDL bridge's semantic operations, not its MT7925
// hardware ABI. Transport is injected so the production path is tested offline.
namespace mt7922 {
using mt7925::Bytes;
using mt7925::Reply;
using mt7925::le16;
using mt7925::le32;
using mt7925::put32;
constexpr uint32_t Start=0x22e40c,Service=0x22e410,Receive=0x226414,
    Transmit=0x22e418,Channel=0x22e41c,Status=0x226420,Address=0x22e424,Stop=0x22a408;
constexpr size_t StatusBytes=256,ReceiveBytes=16392;
inline int32_t signed32(uint32_t n) {
  return n<=0x7fffffffu?int32_t(n):-1-int32_t(~n);
}
inline void require(bool condition,const char* reason) {
  if(!condition)throw std::runtime_error(reason);
}
struct Config {
  std::array<uint8_t,2> country{};
  std::array<uint8_t,6> mac{};
  uint32_t environment=0,power_type=0,primary=0,center=0,width=20,fcs=0,tx=1;
  int32_t power=0;
};
inline Bytes start_request(std::span<const uint8_t> patch,std::span<const uint8_t> ram,const Config& c) {
  require(!patch.empty() && patch.size()<=8*1024*1024 && !ram.empty() && ram.size()<=8*1024*1024,
          "invalid MT7922 firmware lengths");
  require(((c.country[0]=='0' && c.country[1]=='0') ||
          (c.country[0]>='A' && c.country[0]<='Z' && c.country[1]>='A' && c.country[1]<='Z')) &&
          !(c.mac[0]&1) && std::any_of(c.mac.begin(),c.mac.end(),[](auto b){return b!=0;}) &&
          c.environment<=2 && c.power_type<=4 && c.primary && c.primary<=177 && c.center && c.center<=177 &&
          (c.width==20 || c.width==40 || c.width==80) && c.fcs<=1 && c.tx<=1 && c.power>=-127 && c.power<=44,
          "invalid explicit MT7922 configuration");
  Bytes p(64);put32(p,0,0x3252544d);put32(p,4,1);
  put32(p,8,uint32_t(patch.size()));put32(p,12,uint32_t(ram.size()));
  std::copy(c.country.begin(),c.country.end(),p.begin()+16);std::copy(c.mac.begin(),c.mac.end(),p.begin()+18);
  put32(p,24,c.environment);put32(p,28,c.power_type);put32(p,32,c.primary);put32(p,36,c.center);put32(p,40,c.width);
  put32(p,44,c.fcs);put32(p,48,c.tx);put32(p,52,uint32_t(c.power));put32(p,56,1);
  p.insert(p.end(),patch.begin(),patch.end());p.insert(p.end(),ram.begin(),ram.end());return p;
}
inline void validate_start(std::span<const uint8_t> p) {
  require(p.size()>=66 && p.size()<=64+2*8*1024*1024,"invalid MT7922 start length");
  require(le32(p,0)==0x3252544d && le32(p,4)==1 && le32(p,56)==1 && !le32(p,60),"invalid MT7922 start envelope");
  const size_t ps=le32(p,8),rs=le32(p,12);
  require(ps && rs && ps<=8*1024*1024 && rs<=8*1024*1024 && ps+rs==p.size()-64,"invalid firmware partition");
  Config c;std::copy_n(p.begin()+16,2,c.country.begin());std::copy_n(p.begin()+18,6,c.mac.begin());
  c.environment=le32(p,24);c.power_type=le32(p,28);c.primary=le32(p,32);c.center=le32(p,36);c.width=le32(p,40);
  c.fcs=le32(p,44);c.tx=le32(p,48);c.power=signed32(le32(p,52));
  // Reuse validation with tiny placeholder images: no second firmware copy.
  const std::array<uint8_t,1> placeholder{};(void)start_request(placeholder,placeholder,c);
}
inline Bytes translate(uint32_t operation,std::span<const uint8_t> p) {
  if(operation==mt7925::Start){validate_start(p);return Bytes(p.begin(),p.end());}
  if(operation==mt7925::Channel) {
    require(p.size()==16 && le32(p,0)==1,"invalid channel request");
    const auto primary=le32(p,4),center=le32(p,8),width=le32(p,12);
    require(primary && primary<=177 && center && center<=177 && (width==20 || width==40 || width==80),"invalid channel values");
    return Bytes(p.begin()+4,p.end());
  }
  if(operation==mt7925::Address) {
    require(p.size()==12 && le32(p,0)==1 && !p[10] && !p[11],"invalid address request");
    (void)mt7925::address_request(p.subspan(4,6));return Bytes(p.begin()+4,p.begin()+10);
  }
  if(operation==mt7925::Tx) {
    require(p.size()>=40 && p.size()<=16+4095 && le32(p,0)==1 && le32(p,12)==p.size()-16 && le32(p,8)<=1,
            "invalid TX request");
    constexpr std::array<uint32_t,8> rates={12,18,24,36,48,72,96,108};
    const auto index=le32(p,4);require(index>=18 && index<=25,"invalid TX rate");
    Bytes result(12);put32(result,0,le32(p,8));put32(result,4,rates[index-18]);put32(result,8,le32(p,12));
    result.insert(result.end(),p.begin()+16,p.end());return result;
  }
  require((operation==mt7925::Status || operation==mt7925::Poll || operation==mt7925::Stop) && p.empty(),
          "unknown operation or unexpected payload");return {};
}
inline Reply decode_status(std::span<const uint8_t> p,uint32_t operation) {
  require(p.size()==StatusBytes && le32(p,0)==0x3253524d && le32(p,4)==1 && le32(p,8)==StatusBytes,
          "invalid MT7922 radio status envelope");
  const uint32_t flags=le32(p,12),nt=le32(p,28),token_valid=le32(p,212);
  require(!(flags&~2047u) && le32(p,16)<=12 && token_valid<=1 &&
          std::all_of(p.begin()+216,p.end(),[](auto b){return b==0;}),"invalid MT7922 status fields");
  Reply r;r.result=signed32(le32(p,20));r.stop_error=signed32(le32(p,24));r.stage=le32(p,16);
  r.ready=(flags&4)!=0;r.attempted=(flags&1024)!=0;r.stopped=(flags&8)!=0;
  r.published=(flags&512)!=0;r.quiescent=(flags&128)!=0;
  r.primary=le32(p,36);r.center=le32(p,40);r.width=le32(p,44);r.inflight=le32(p,80);
  std::copy_n(p.begin()+50,6,r.mac.begin());
  require(!r.ready || ((flags&(1|2|32|64|1024))==(1|2|32|64|1024) &&
          !(flags&(8|128|256)) && r.published && r.stage==12 && !r.stop_error && le32(p,72)==1),
          "inconsistent MT7922 readiness");
  if((nt&0x80000000u) && !r.result)r.result=-1000; // E.g. hash failure before core entry.
  r.error=(!r.ready || r.result < -3 || r.result>0)?r.result:0;
  if(!r.ready && !r.error)r.error=-1000;
  require(token_valid?operation==mt7925::Tx && !r.result && le32(p,208)<256:!le32(p,208),"invalid TX token envelope");
  if(operation==mt7925::Tx && !r.result)require(token_valid!=0,"missing accepted TX token");
  if(token_valid)r.token=le32(p,208);
  // The shared bridge retries FULL for TX. Neither FULL nor PENDING transfers
  // ownership; map only the healthy, explicitly rejected PENDING result.
  if(operation==mt7925::Tx && r.ready && !r.error && r.result==-3)r.result=-2;
  return r;
}
inline Reply decode_stop(std::span<const uint8_t> p) {
  require(p.size()==128 && le32(p,0)==0x3253544d && le32(p,4)==1 && le32(p,8)==128,"invalid STOP envelope");
  const auto flags=le32(p,12);require(!(flags&~255u),"invalid STOP flags");
  Reply r;r.result=(le32(p,16)&0x80000000u)?-1000:0;r.stop_error=signed32(le32(p,24));
  r.attempted=(flags&8)!=0;r.published=(flags&64)!=0;r.quiescent=(flags&128)!=0;
  r.stopped=!r.result && !r.stop_error && (!r.published || r.quiescent);
  // Offset 20 is the prior operation's core error, not STOP's result.
  if(!r.stopped && !r.result)r.result=-1000;r.error=r.result;return r;
}
inline void validate_record(std::span<const uint8_t> p) {
  require(p.size()>=8+8+24 && p.size()<=ReceiveBytes && le16(p,0)==2 && !le16(p,2) &&
          le32(p,4)==p.size()-8 && !p[8] && !p[9],"invalid RX record envelope");
  const auto rt=le16(p,10);require(rt>=8 && rt<=p.size()-8-24,"invalid RX radiotap length");
}
struct IoResult {uint32_t error=0;Bytes bytes;};
// Transport::exchange(code, input, output_capacity) must bound returned bytes.
// Caller serializes this adapter (the AWDL bridge does so for all operations).
template<class Transport> class Adapter {
  Transport& transport_;
public:
  explicit Adapter(Transport& transport):transport_(transport){}
  Reply call(uint32_t operation,std::span<const uint8_t> input={}) {
    const auto request=translate(operation,input);uint32_t code=0;
    switch(operation) {
      case mt7925::Start:code=Start;break;case mt7925::Status:code=Status;break;
      case mt7925::Channel:code=Channel;break;case mt7925::Tx:code=Transmit;break;
      case mt7925::Poll:code=Service;break;case mt7925::Stop:code=Stop;break;
      case mt7925::Address:code=Address;break;default:throw std::runtime_error("unknown operation");
    }
    auto io=transport_.exchange(code,request,operation==mt7925::Stop?128:StatusBytes);
    if(io.error)throw std::runtime_error("MT7922 IOCTL Windows error "+std::to_string(io.error));
    if(operation==mt7925::Stop)return decode_stop(io.bytes);
    auto r=decode_status(io.bytes,operation);
    if(operation==mt7925::Poll && !r.result && r.ready && !r.error && !r.stop_error) {
      io=transport_.exchange(Receive,{},ReceiveBytes);
      if(io.error==259) {require(io.bytes.empty(),"empty RECEIVE returned bytes");r.result=-3;}
      else {
        if(io.error)throw std::runtime_error("MT7922 RECEIVE Windows error "+std::to_string(io.error));
        validate_record(io.bytes);r.record=std::move(io.bytes);
      }
    }
    return r;
  }
};
} // namespace mt7922
