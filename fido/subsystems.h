// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "retro/screen.h"
#include "retro/table_window.h"
#include "fido/event_mux.h"
#include "common/event.h"
#include "common/subsystem_status.h"
#include "absl/container/flat_hash_map.h"
#include <string>

namespace adastra::fido {

class SubsystemsWindow : public retro::TableWindow {
public:
  SubsystemsWindow(retro::Screen *screen, EventMux& mux);
  ~SubsystemsWindow() = default;

  void ApplyFilter() override;

private:
  void RunnerCoroutine(co::Coroutine *c) override;
  void PopulateTable();

  EventMux& mux_;
  toolbelt::SharedPtrPipe<adastra::Event> event_pipe_;
  absl::flat_hash_map<std::string, adastra::SubsystemStatus> subsystems_;
};

} // namespace adastra::fido