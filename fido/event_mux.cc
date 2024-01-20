#include "fido/event_mux.h"
#include "fido/fido.h"

namespace fido {

EventMux::EventMux(Application &app, toolbelt::InetAddress flight_addr)
    : app_(app), flight_addr_(flight_addr),
      client_(stagezero::flight::client::ClientMode::kBlocking) {}

absl::Status EventMux::Init() {
  if (absl::Status status = client_.Init(flight_addr_, "FDO",
                                         stagezero::kSubsystemStatusEvents |
                                             stagezero::kLogMessageEvents |
                                             stagezero::kAlarmEvents);
      !status.ok()) {
    return absl::InternalError(
        absl::StrFormat("Can't connect to FlightDirector at address %s: %s",
                        flight_addr_.ToString(), status.ToString()));
  }
  app_.AddCoroutine(std::make_unique<co::Coroutine>(
      app_.Scheduler(), [this](co::Coroutine *c) { RunnerCoroutine(c); }));
  return absl::OkStatus();
}

// Read events from flight and distribute them to the outputs.
void EventMux::RunnerCoroutine(co::Coroutine *c) {
  for (;;) {
    absl::StatusOr<std::shared_ptr<stagezero::Event>> event =
        client_.WaitForEvent(c);
    if (!event.ok()) {
      // Report an error
      return;
    }
    for (auto &output : outputs_) {
      absl::Status status = output->Write(*event);
      if (!status.ok()) {
        // Report error
      }
    }
  }
}

} // namespace fido