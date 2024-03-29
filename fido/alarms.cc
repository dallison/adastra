// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "fido/alarms.h"
#include "toolbelt/triggerfd.h"

namespace adastra::fido {

AlarmsWindow::AlarmsWindow(retro::Screen *screen, EventMux &mux)
    : TableWindow(screen,
                  {.title = "[4] Current Alarms",
                   .nlines = 16,
                   .ncols = screen->Width() * 7 / 16,
                   .y = 17,
                   .x = screen->Width() * 9 / 16},
                  {"name", "type", "severity", "reason", "details"}),
      mux_(mux) {
  // What is a good width for the details column?  Use the expected max widths
  // for the other columns to work it out.  It will wrap but we want to avoid
  // unnecessary wraps.
  int kDetailsWidth = Width() - (16 + 10 + 9 + 9);
  display_table_.SetWrapColumn(4, kDetailsWidth);
}

void AlarmsWindow::RunnerCoroutine(co::Coroutine *c) {
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
      connected = false;
      continue;
    }
    auto event = std::move(*pevent);
    if (event->type != adastra::EventType::kAlarm) {
      continue;
    }
    auto alarm = std::get<1>(event->event);
    if (alarm.status == adastra::Alarm::Status::kCleared) {
      alarms_.erase(alarm.id);
    } else {
      auto &a = alarms_[alarm.id];
      a = alarm;
    }
    PopulateTable();
  }
}

static const char *AlarmType(adastra::Alarm::Type type) {
  switch (type) {
  case adastra::Alarm::Type::kProcess:
    return "process";
  case adastra::Alarm::Type::kSubsystem:
    return "subsystem";
  case adastra::Alarm::Type::kSystem:
    return "system";
  case adastra::Alarm::Type::kUnknown:
    return "unknown";
  }
}

static const char *AlarmSeverity(adastra::Alarm::Severity s) {
  switch (s) {
  case adastra::Alarm::Severity::kWarning:
    return "warning";
  case adastra::Alarm::Severity::kError:
    return "error";
  case adastra::Alarm::Severity::kCritical:
    return "critical";
  case adastra::Alarm::Severity::kUnknown:
    return "unknown";
  }
}

static const char *AlarmReason(adastra::Alarm::Reason r) {
  switch (r) {
  case adastra::Alarm::Reason::kCrashed:
    return "crashed";
  case adastra::Alarm::Reason::kBroken:
    return "broken";
  case adastra::Alarm::Reason::kEmergencyAbort:
    return "abort";
  case adastra::Alarm::Reason::kUnknown:
    return "unknown";
  }
}

static int ColorForSeverity(adastra::Alarm::Severity s) {
  switch (s) {
  case adastra::Alarm::Severity::kWarning:
    return retro::kColorPairCyan;
  case adastra::Alarm::Severity::kError:
    return retro::kColorPairMagenta;
  case adastra::Alarm::Severity::kCritical:
    return retro::kColorPairRed;
  case adastra::Alarm::Severity::kUnknown:
    return retro::kColorPairNormal;
  }
}

void AlarmsWindow::ApplyFilter() { PopulateTable(); }

void AlarmsWindow::PopulateTable() {
  retro::Table &table = display_table_;
  table.Clear();
  for (auto & [ name, alarm ] : alarms_) {
    if (!display_filter_.empty() &&
        alarm.name.find(display_filter_) == std::string::npos) {
      continue;
    }
    table.AddRow();

    int color = ColorForSeverity(alarm.severity);
    table.SetCell(0, retro::Table::MakeCell(alarm.name, color));
    table.SetCell(1, retro::Table::MakeCell(AlarmType(alarm.type), color));
    table.SetCell(2,
                  retro::Table::MakeCell(AlarmSeverity(alarm.severity), color));
    table.SetCell(3, retro::Table::MakeCell(AlarmReason(alarm.reason), color));
    table.SetCell(4, retro::Table::MakeCell(alarm.details, color));
  }
  Draw();
}

} // namespace adastra::fido
