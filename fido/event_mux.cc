#include "fido/event_mux.h"
#include "fido/fido.h"

namespace adastra::fido {

EventMux::EventMux(retro::Application &app, toolbelt::InetAddress flight_addr)
    : app_(app), flight_addr_(flight_addr) {}

void EventMux::Init() {
  app_.AddCoroutine(std::make_unique<co::Coroutine>(
      app_.Scheduler(), [this](co::Coroutine *c) { RunnerCoroutine(c); }));
}

void EventMux::AddListener(std::function<void(MuxStatus)> listener) {
  if (client_ != nullptr) {
    listener(MuxStatus::kConnected);
  }
  listeners_.push_back(std::move(listener));
}

void EventMux::AddSink(toolbelt::SharedPtrPipe<adastra::Event> *sink) {
  sinks_.push_back(sink);
}

void EventMux::NotifyListeners(MuxStatus status) {
  for (auto& listener : listeners_) {
    listener(status);
  }
}

// Read events from flight and distribute them to the outputs.
void EventMux::RunnerCoroutine(co::Coroutine *c) {
  bool connected = false;
  for (;;) {
    if (!connected) {
      client_ = std::make_unique<adastra::flight::client::Client>(
          adastra::flight::client::ClientMode::kBlocking);

      if (absl::Status status = client_->Init(
              flight_addr_, "FDO",
              adastra::kSubsystemStatusEvents | adastra::kLogMessageEvents |
                  adastra::kAlarmEvents);
          !status.ok()) {
        client_.reset();
        c->Sleep(2);
        continue;
      }
      NotifyListeners(MuxStatus::kConnected);
      connected = true;
    }
    absl::StatusOr<std::shared_ptr<adastra::Event>> event =
        client_->WaitForEvent(c);
    if (!event.ok()) {
      client_.reset();
      NotifyListeners(MuxStatus::kDisconnected);
      c->Sleep(2);
      connected = false;
      continue;
    }
    for (auto &sink : sinks_) {
      absl::Status status = sink->Write(*event);
      if (!status.ok()) {
        return;
      }
    }
  }
}

} // namespace adastra::fido
