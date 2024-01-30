// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "fido/processes.h"
#include "absl/strings/str_format.h"
#include "retro/screen.h"
#include "toolbelt/triggerfd.h"

namespace adastra::fido {

ProcessesWindow::ProcessesWindow(retro::Screen *screen, EventMux &mux)
    : TableWindow(screen,
                  {.title = "[2] Running Processes",
                   .nlines = 16,
                   .ncols = screen->Width() / 2,
                   .y = 1,
                   .x = screen->Width() / 2},
                  {"name", "type", "subsystem", "compute", "pid", "alarms"}),
      mux_(mux) {}

void ProcessesWindow::RunnerCoroutine(co::Coroutine *c) {
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
    absl::StatusOr<std::shared_ptr<adastra::Event>> pevent =
        event_pipe_.Read();
    if (!pevent.ok()) {
      // Print an error.
      connected = false;
      continue;
    }
    auto event = std::move(*pevent);
    if (event->type != adastra::EventType::kSubsystemStatus) {
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

const char *ProcessType(adastra::ProcessType t) {
  switch (t) {
  case adastra::ProcessType::kStatic:
    return "static";
  case adastra::ProcessType::kZygote:
    return "zygote";
  case adastra::ProcessType::kVirtual:
    return "virtual";
  }
}

void ProcessesWindow::ApplyFilter() { PopulateTable(); }

void ProcessesWindow::PopulateTable() {
  retro::Table &table = display_table_;
  table.Clear();
  for (auto & [ name, proc ] : processes_) {
    if (!display_filter_.empty() &&
        name.find(display_filter_) == std::string::npos) {
      continue;
    }
    table.AddRow();

    int color = proc.alarm_count == 0 ? retro::kColorPairNormal
                                      : retro::kColorPairMagenta;
    table.SetCell(0, retro::Table::MakeCell(proc.name, color));
    table.SetCell(1, retro::Table::MakeCell(ProcessType(proc.type), color));
    table.SetCell(2, retro::Table::MakeCell(proc.subsystem, color));
    table.SetCell(3, retro::Table::MakeCell(proc.compute, color));
    table.SetCell(
        4, retro::Table::MakeCell(absl::StrFormat("%d", proc.pid), color));
    table.SetCell(5, retro::Table::MakeCell(
                         absl::StrFormat("%d", proc.alarm_count), color));
  }
  Draw();
}

} // namespace adastra::fido
