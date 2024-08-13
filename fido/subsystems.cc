// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "fido/subsystems.h"
#include "absl/strings/str_format.h"
#include "retro/screen.h"
#include "toolbelt/triggerfd.h"

namespace adastra::fido {

SubsystemsWindow::SubsystemsWindow(retro::Screen *screen, EventMux &mux)
    : TableWindow(screen,
                  {.title = "[1] Subsystems",
                   .nlines = 16,
                   .ncols = screen->Width() / 2 - 1,
                   .y = 1,
                   .x = 0},
                  {"name", "admin", "oper", "processes", "alarms", "restarts"}),
      mux_(mux) {}

void SubsystemsWindow::RunnerCoroutine(co::Coroutine *c) {
  bool connected = false;

  auto p = toolbelt::SharedPtrPipe<adastra::Event>::Create();
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
    int fd = c->Wait({event_pipe_.ReadFd().Fd(), interrupt.GetPollFd().Fd()},
                     POLLIN);
    if (fd == interrupt.GetPollFd().Fd()) {
      interrupt.Clear();
      continue;
    }
    absl::StatusOr<std::shared_ptr<adastra::Event>> pevent = event_pipe_.Read();
    if (!pevent.ok()) {
      connected = false;
      continue;
    }
    auto event = std::move(*pevent);
    if (event->type != adastra::EventType::kSubsystemStatus) {
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
    case adastra::AdminState::kOffline:
      admin_color = retro::kColorPairMagenta;
      break;
    case adastra::AdminState::kOnline:
      admin_color = retro::kColorPairGreen;
      break;
    }

    switch (subsystem.oper_state) {
    case adastra::OperState::kOffline:
      oper_color = retro::kColorPairMagenta;
      break;
    case adastra::OperState::kBroken:
    case adastra::OperState::kDegraded:
      oper_color = retro::kColorPairRed;
      break;
    case adastra::OperState::kOnline:
      oper_color = retro::kColorPairGreen;
      break;
    case adastra::OperState::kRestarting:
    case adastra::OperState::kStartingChildren:
    case adastra::OperState::kStartingProcesses:
    case adastra::OperState::kRestartingProcesses:
    case adastra::OperState::kStoppingChildren:
    case adastra::OperState::kStoppingProcesses:
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

} // namespace adastra::fido
