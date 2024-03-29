#pragma once

#include "flight/client/client.h"
#include "toolbelt/pipe.h"
#include "toolbelt/sockets.h"
#include <vector>
#include "retro/app.h"

#include <functional>

namespace adastra::fido {

class Application;

enum class MuxStatus {
  kConnected,
  kDisconnected,
};

class EventMux {
public:
  EventMux(retro::Application& app, toolbelt::InetAddress flight_addr);
  ~EventMux() = default;

  void Init();
  
  void AddListener(std::function<void(MuxStatus)> callback);
  void AddSink(toolbelt::SharedPtrPipe<adastra::Event>* sink);

private:
  void RunnerCoroutine(co::Coroutine* c);
  void NotifyListeners(MuxStatus status);

  retro::Application& app_;
  toolbelt::InetAddress flight_addr_;
  std::unique_ptr<adastra::flight::client::Client> client_;
  std::vector<std::function<void(MuxStatus)>> listeners_;
  std::vector<toolbelt::SharedPtrPipe<adastra::Event>*> sinks_;
};

} // namespace adastra::fido
