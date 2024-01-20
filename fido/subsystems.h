#pragma once

#include "fido/screen.h"
#include "fido/table_window.h"
#include "fido/event_mux.h"
#include "common/event.h"
#include "common/subsystem_status.h"
#include "absl/container/flat_hash_map.h"
#include <string>

namespace fido {

class SubsystemsWindow : public TableWindow {
public:
  SubsystemsWindow(Screen *screen, EventMux& mux);
  ~SubsystemsWindow() = default;

private:
  void RunnerCoroutine(co::Coroutine *c) override;
  void PopulateTable();

  toolbelt::SharedPtrPipe<stagezero::Event> event_pipe_;
  absl::flat_hash_map<std::string, stagezero::SubsystemStatus> subsystems_;
};

} // namespace fido