// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "retro/screen.h"
#include "retro/table_window.h"
#include "fido/event_mux.h"
#include "common/event.h"
#include "common/alarm.h"
#include "absl/container/flat_hash_map.h"

namespace fido {

class AlarmsWindow : public retro::TableWindow {
public:
  AlarmsWindow(retro::Screen *screen, EventMux& mux);
  ~AlarmsWindow() = default;

  void ApplyFilter() override;

private:
  void RunnerCoroutine(co::Coroutine *c) override;
  void PopulateTable();

  EventMux& mux_;
  toolbelt::SharedPtrPipe<stagezero::Event> event_pipe_;
  absl::flat_hash_map<std::string, stagezero::Alarm> alarms_;
};

} // namespace fido