#pragma once

#include "flight/client/client.h"
#include "toolbelt/pipe.h"
#include "toolbelt/sockets.h"
#include <vector>

namespace fido {

class Application;

class EventMux {
public:
  EventMux(Application& app, toolbelt::InetAddress flight_addr);
  ~EventMux() = default;

  absl::Status Init();
  
  void AddOutput(toolbelt::SharedPtrPipe<stagezero::Event>* output) {
    outputs_.push_back(std::move(output));
  }

private:
  void RunnerCoroutine(co::Coroutine* c);

  Application& app_;
  toolbelt::InetAddress flight_addr_;
  stagezero::flight::client::Client client_;
  std::vector<toolbelt::SharedPtrPipe<stagezero::Event>*> outputs_;
};

} // namespace fido