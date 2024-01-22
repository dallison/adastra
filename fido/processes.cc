#include "fido/processes.h"
#include "absl/strings/str_format.h"
#include "fido/screen.h"

namespace fido {

ProcessesWindow::ProcessesWindow(Screen *screen, EventMux &mux)
    : TableWindow(screen,
                  {.title = "Running Processes",
                   .nlines = 16,
                   .ncols = screen->Width() / 2,
                   .x = screen->Width() / 2,
                   .y = 1},
                  {"name", "type", "subsystem", "compute", "pid", "alarms"}) {
  auto p = toolbelt::SharedPtrPipe<stagezero::Event>::Create();
  if (!p.ok()) {
    std::cerr << "Failed to create event pipe: " << strerror(errno)
              << std::endl;
  }
  event_pipe_ = std::move(*p);
  mux.AddOutput(&event_pipe_);
}

void ProcessesWindow::RunnerCoroutine(co::Coroutine *c) {
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

    for (auto &proc : subsystem.processes) {
      if (!proc.running) {
        if (!proc.process_id.empty()) {
          processes_.erase(proc.process_id);
        }
        continue;
      }

      auto &status = processes_[proc.process_id];
      status = proc;
    }

    // Update the table with the new data.
    PopulateTable();
  }
}

const char *ProcessType(stagezero::ProcessType t) {
  switch (t) {
  case stagezero::ProcessType::kStatic:
    return "static";
  case stagezero::ProcessType::kZygote:
    return "zygote";
  case stagezero::ProcessType::kVirtual:
    return "virtual";
  }
}

void ProcessesWindow::PopulateTable() {
  Table &table = display_table_;
  table.Clear();
  for (auto & [ name, proc ] : processes_) {
    table.AddRow();

    int color = proc.alarm_count == 0 ? kColorPairNormal : kColorPairMagenta;
    table.SetCell(0, Table::MakeCell(proc.name, color));
    table.SetCell(1, Table::MakeCell(ProcessType(proc.type), color));
    table.SetCell(2, Table::MakeCell(proc.subsystem, color));
    table.SetCell(3, Table::MakeCell(proc.compute, color));
    table.SetCell(4, Table::MakeCell(absl::StrFormat("%d", proc.pid), color));
    table.SetCell(5, Table::MakeCell(absl::StrFormat("%d", proc.alarm_count), color));
  }
  Draw();
}

} // namespace fido