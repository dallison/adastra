#include "fido/subsystems.h"
#include "absl/strings/str_format.h"
#include "fido/screen.h"

namespace fido {

SubsystemsWindow::SubsystemsWindow(Screen *screen, EventMux &mux)
    : TableWindow(
          screen,
          {.title = "Subsystems",
           .nlines = 16,
           .ncols = screen->Width() / 2 - 1,
           .x = 0,
           .y = 1},
          {"name", "admin", "oper", "processes", "alarms", "restarts"}) {
  auto p = toolbelt::SharedPtrPipe<stagezero::Event>::Create();
  if (!p.ok()) {
    std::cerr << "Failed to create event pipe: " << strerror(errno)
              << std::endl;
  }
  event_pipe_ = std::move(*p);
  mux.AddOutput(&event_pipe_);
}

void SubsystemsWindow::RunnerCoroutine(co::Coroutine *c) {
  for (;;) {
    // Wait for incoming event.
    c->Wait(event_pipe_.ReadFd().Fd(), POLLIN);
    absl::StatusOr<std::shared_ptr<stagezero::Event>> pevent =
        event_pipe_.Read();
    if (!pevent.ok()) {
      // Print an error.
      return;
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

void SubsystemsWindow::PopulateTable() {
  Table &table = display_table_;
  table.Clear();
  for (auto & [ name, subsystem ] : subsystems_) {
    table.AddRow();
    int admin_color;
    int oper_color;

    switch (subsystem.admin_state) {
    case stagezero::AdminState::kOffline:
      admin_color = kColorPairMagenta;
      break;
    case stagezero::AdminState::kOnline:
      admin_color = kColorPairGreen;
      break;
    }

    switch (subsystem.oper_state) {
    case stagezero::OperState::kOffline:
      oper_color = kColorPairMagenta;
      break;
    case stagezero::OperState::kBroken:
      oper_color = kColorPairRed;
      break;
    case stagezero::OperState::kOnline:
      oper_color = kColorPairGreen;
      break;
    case stagezero::OperState::kRestarting:
    case stagezero::OperState::kStartingChildren:
    case stagezero::OperState::kStartingProcesses:
    case stagezero::OperState::kStoppingChildren:
    case stagezero::OperState::kStoppingProcesses:
      oper_color = kColorPairYellow;
      break;
    }
    table.SetCell(0, Table::MakeCell(subsystem.subsystem));
    table.SetCell(
        1, Table::MakeCell(AdminStateName(subsystem.admin_state), admin_color));
    table.SetCell(
        2, Table::MakeCell(OperStateName(subsystem.oper_state), oper_color));
    table.SetCell(
        3, Table::MakeCell(absl::StrFormat("%d", subsystem.processes.size())));
    table.SetCell(
        4, Table::MakeCell(absl::StrFormat("%d", subsystem.alarm_count)));
    table.SetCell(
        5, Table::MakeCell(absl::StrFormat("%d", subsystem.restart_count)));
  }
  Draw();
}

} // namespace fido