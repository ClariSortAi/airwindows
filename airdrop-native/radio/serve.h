// SPDX-License-Identifier: GPL-2.0-only
// Derived from the pinned bridge serve loop; see SOURCES.md.
#pragma once
#include "secure_pipe.h"
#include "mt7925_bridge.h"
namespace mt7922 {
using mt7925::PollPause;
using mt7925::StopRequested;
using mt7925::receive_loop;
// A probe that sends no complete command may reconnect. Once a client sends
// commands, departure ends this one hardware generation. No automatic restart.
template<class Session> void serve(SecurePipeServer& pipe,Session& session,uint8_t primary,
                                  std::atomic<bool>& stopped) {
  std::mutex route;
  bool deliver=false;
  std::exception_ptr worker_error;
  std::thread worker([&] {
    try {
      PollPause pause;
      receive_loop(session,stopped,[&](const Bytes& rx) {
          std::lock_guard lock(route);
          if(deliver && !pipe.send(RADIO_LINK_RX,rx,500)) {
            if(!stopped)throw std::runtime_error("client RX pipe closed or stalled");
          }
      },pause);
    } catch(...) {worker_error=std::current_exception();stopped=true;}
  });
  std::exception_ptr reader_error;
  try {
    while(!stopped) {
      if(!pipe.wait_for_client()) {if(!stopped)throw std::runtime_error("pipe accept failed");break;}
      bool hello=false,active=false;
      {
        std::lock_guard lock(route);
        const radio_link_hello h{RADIO_LINK_VERSION,primary,20,{0,0}};
        hello=pipe.send(RADIO_LINK_HELLO,{reinterpret_cast<const uint8_t*>(&h),sizeof(h)},500);
      }
      while(hello && !stopped) {
        radio_link_header h{};
        if(!pipe.read_exact(&h,sizeof(h)))break;
        active=true;
        Bytes p(Session::length(h));
        if(!pipe.read_exact(p.data(),static_cast<DWORD>(p.size()))) {
          if(stopped)throw StopRequested();
          throw std::runtime_error("truncated client command");
        }
        session.command(h,p);
        std::lock_guard lock(route);deliver=true;
      }
      {std::lock_guard lock(route);deliver=false;pipe.drop_client();}
      if(active)break;
    }
  } catch(...) {reader_error=std::current_exception();}
  stopped=true;worker.join();
  if(worker_error)std::rethrow_exception(worker_error);
  if(reader_error)std::rethrow_exception(reader_error);
}
} // namespace mt7922
