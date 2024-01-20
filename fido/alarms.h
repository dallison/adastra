#pragma once

#include "fido/screen.h"
#include "fido/table_window.h"
#include "fido/event_mux.h"
#include "common/event.h"
#include "common/alarm.h"
#include "absl/container/flat_hash_map.h"

namespace fido {

class AlarmsWindow : public TableWindow {
public:
  AlarmsWindow(Screen *screen, EventMux& mux);
  ~AlarmsWindow() = default;

private:
  void RunnerCoroutine(co::Coroutine *c) override;
  void PopulateTable();

  toolbelt::SharedPtrPipe<stagezero::Event> event_pipe_;
  absl::flat_hash_map<std::string, stagezero::Alarm> alarms_;
};

} // namespace fido