// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "fido/subsystems.h"
#include "absl/strings/str_format.h"
#include "retro/screen.h"
#include "toolbelt/triggerfd.h"

namespace fido {

SubsystemsWindow::SubsystemsWindow(retro::Screen *screen, EventMux &mux)
    : TableWindow(screen,
                  {.title = "[1] Subsystems",
                   .nlines = 16,
                   .ncols = screen->Width() / 2 - 1,
                   .x = 0,
                   .y = 1},
                  {"name", "admin", "oper", "processes", "alarms", "restarts"}),
      mux_(mux) {}

void SubsystemsWindow::RunnerCoroutine(co::Coroutine *c) {
  bool connected = false;

  auto p = toolbelt::SharedPtrPipe<stagezero::Event>::Create();
  if (!p.ok()) {
    return;
  }

  event_pipe_ = std::move(*p);
  mux_.AddSink(&event_pipe_);

  toolbelt::TriggerFd interrupt;
  if (absl::Status status = interrupt.Open(); !status.ok()) {
    return;
  }
  mux_.AddListener([&connected, &interrupt](MuxStatus s) {
    connected = s == MuxStatus::kConnected;
    interrupt.Trigger();
  });

  for (;;) {
    if (!connected) {
      DrawErrorBanner("CONNECTING TO FLIGHT");
      c->Sleep(2);
      display_table_.Clear();
      Draw();
      continue;
    }
    // Wait for incoming event.
    int fd = c->Wait({event_pipe_.ReadFd().Fd(), interrupt.GetPollFd().Fd()}, POLLIN);
    if (fd == interrupt.GetPollFd().Fd()) {
      interrupt.Clear();
      continue;
    }
    absl::StatusOr<std::shared_ptr<stagezero::Event>> pevent =
        event_pipe_.Read();
    if (!pevent.ok()) {
      connected = false;
      continue;
    }
    auto event = std::move(*pevent);
    if (event->type != stagezero::EventType::kSubsystemStatus) {
      continue;
    }
    auto subsystem = std::get<0>(event->event);

    // Add subsystem status to the local map.
    auto &status = subsystems_[subsystem.subsystem];
    status = subsystem;

    // Update the table with the new data.
    PopulateTable();
  }
}

void SubsystemsWindow::ApplyFilter() { PopulateTable(); }

void SubsystemsWindow::PopulateTable() {
  retro::Table &table = display_table_;
  table.Clear();
  for (auto & [ name, subsystem ] : subsystems_) {
    if (!display_filter_.empty() &&
        name.find(display_filter_) == std::string::npos) {
      continue;
    }
    table.AddRow();
    int admin_color;
    int oper_color;

    switch (subsystem.admin_state) {
    case stagezero::AdminState::kOffline:
      admin_color = retro::kColorPairMagenta;
      break;
    case stagezero::AdminState::kOnline:
      admin_color = retro::kColorPairGreen;
      break;
    }

    switch (subsystem.oper_state) {
    case stagezero::OperState::kOffline:
      oper_color = retro::kColorPairMagenta;
      break;
    case stagezero::OperState::kBroken:
      oper_color = retro::kColorPairRed;
      break;
    case stagezero::OperState::kOnline:
      oper_color = retro::kColorPairGreen;
      break;
    case stagezero::OperState::kRestarting:
    case stagezero::OperState::kStartingChildren:
    case stagezero::OperState::kStartingProcesses:
    case stagezero::OperState::kStoppingChildren:
    case stagezero::OperState::kStoppingProcesses:
      oper_color = retro::kColorPairYellow;
      break;
    }
    table.SetCell(0, retro::Table::MakeCell(subsystem.subsystem));
    table.SetCell(1, retro::Table::MakeCell(
                         AdminStateName(subsystem.admin_state), admin_color));
    table.SetCell(2, retro::Table::MakeCell(OperStateName(subsystem.oper_state),
                                            oper_color));
    table.SetCell(3, retro::Table::MakeCell(
                         absl::StrFormat("%d", subsystem.processes.size())));
    table.SetCell(4, retro::Table::MakeCell(
                         absl::StrFormat("%d", subsystem.alarm_count)));
    table.SetCell(5, retro::Table::MakeCell(
                         absl::StrFormat("%d", subsystem.restart_count)));
  }
  Draw();
}

} // namespace fido