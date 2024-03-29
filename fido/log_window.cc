// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "fido/log_window.h"
#include "fido/fido.h"
#include "toolbelt/triggerfd.h"
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

namespace adastra::fido {

LogWindow::LogWindow(retro::Screen *screen, EventMux &mux)
    : Panel(screen, {.title = "[5] Log Messages",
                     .nlines = screen->Height() - 33,
                     .ncols = screen->Width(),
                     .y = 33,
                     .x = 0}),
      mux_(mux) {
  // Divide the window into columns.
  column_widths_[0] = 30; // Timestamp.
  column_widths_[1] = 3;  // Log level
  column_widths_[2] = 20; // Source
  size_t remaining = Width();
  for (int i = 0; i < 3; i++) {
    remaining -= column_widths_[i] + 1;
  }
  column_widths_[3] = remaining - 1;

  colors_[0] = retro::kColorPairCyan;
  // Column 1 color depends on the log level.
  colors_[2] = retro::kColorPairYellow;
  // Column 3 color depends on the log level.
}

void LogWindow::Run() {
  Draw();
  App().AddCoroutine(std::make_unique<co::Coroutine>(
      Scheduler(), [this](co::Coroutine *c) { RunnerCoroutine(c); }));
}

void LogWindow::RunnerCoroutine(co::Coroutine *c) {
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
      logs_.clear();
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
    if (event->type != adastra::EventType::kLog) {
      continue;
    }
    auto log = std::get<3>(event->event);
    logs_.push_back(log);
    Render();
  }
}

void LogWindow::Draw(bool refresh) {
  wborder(win_, 0, 0, 0, 0, 0, 0, 0, 0);
  PrintTitle();
  if (refresh) {
    Refresh();
  }
}

void LogWindow::ApplyFilter() { Render(); }

// We start at the most recent log message and work backwards
// until we fill the window.  Each log message might occupy multiple
// lines.
void LogWindow::Render() {
  int total_rows = 0;
  int available_rows = Height() - 1;
  std::list<MessageLines> lines;
  for (auto it = logs_.rbegin(); it != logs_.rend(); it++) {
    if (it->level < log_level_) {
      // Don't render log message less than our requested level.
      continue;
    }
    if (total_rows >= available_rows) {
      break;
    }
    MessageLines l = RenderMessage(*it);
    lines.push_front(l);
    total_rows += l.num_rows;
  }
  EraseCanvas();

  // We place the lines in the windows from the bottom up.
  int row = 1 + std::min(total_rows, available_rows);

  for (auto it = lines.rbegin(); it != lines.rend(); it++) {
    auto &line = *it;

    // Each field has a row number which is relative to the message lines.  The
    // first is row 0 and each subsequent row increments it.
    // The 'row' variable is where we want to place the last row in the lines.
    for (auto field_it = line.fields.rbegin(); field_it != line.fields.rend();
         field_it++) {
      auto &field = *field_it;

      int dest_row = row - line.num_rows + field.row;
      if (dest_row < 1) {
        // Don't go outside the top of the window (row 0 is the border).
        break;
      }
      Move(dest_row, field.col);
      ColorOn(field.color);
      Print(field.data);
      ColorOff(field.color);
    }
    row -= line.num_rows;
  }
  Draw();
}

static const char *LogLevelAsString(toolbelt::LogLevel level) {
  switch (level) {
  case toolbelt::LogLevel::kVerboseDebug:
    return "V";
  case toolbelt::LogLevel::kDebug:
    return "D";
  case toolbelt::LogLevel::kInfo:
    return "I";
  case toolbelt::LogLevel::kWarning:
    return "W";
  case toolbelt::LogLevel::kError:
    return "E";
  case toolbelt::LogLevel::kFatal:
    return "F";
  }
  return "U";
}

static int ColorForLogLevel(toolbelt::LogLevel level) {
  switch (level) {
  case toolbelt::LogLevel::kVerboseDebug:
    return retro::kColorPairGreen;
  case toolbelt::LogLevel::kDebug:
    return retro::kColorPairGreen;
  case toolbelt::LogLevel::kInfo:
    return retro::kColorPairNormal;
  case toolbelt::LogLevel::kWarning:
    return retro::kColorPairMagenta;
  case toolbelt::LogLevel::kError:
    return retro::kColorPairRed;
  case toolbelt::LogLevel::kFatal:
    return retro::kColorPairRed;
  }

  return retro::kColorPairCyan;
}

LogWindow::MessageLines
LogWindow::RenderMessage(const adastra::LogMessage &msg) {
  MessageLines lines = {.num_rows = 0};
  int col = 0;

  if (!display_filter_.empty()) {
    // We can filter on the source or the text.
    if (msg.source.find(display_filter_) == std::string::npos &&
        msg.text.find(display_filter_) == std::string::npos) {
      return lines;
    }
  }

  // Add the fixed width fields.
  char timebuf[64];
  struct tm tm;
  time_t secs = msg.timestamp / 1000000000LL;
  size_t n = strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S",
                      localtime_r(&secs, &tm));
  snprintf(timebuf + n, sizeof(timebuf) - n, ".%09" PRIu64,
           msg.timestamp % 1000000000);

  colors_[1] = colors_[3] = ColorForLogLevel(msg.level);

  // We don't know the start row until we render the log message as that can
  // take up multiple lines.  We will assign the start rows after the number
  // of rows are known.

  // Field 0: time
  lines.fields.push_back({.col = col, .color = colors_[0], .data = timebuf});
  col += column_widths_[0];

  // Field 1: log level
  lines.fields.push_back(
      {.col = col, .color = colors_[1], .data = LogLevelAsString(msg.level)});
  col += column_widths_[1];

  // Field 2: source.
  lines.fields.push_back({.col = col, .color = colors_[2], .data = msg.source});
  col += column_widths_[2];

  // Now fill in fields 3 and up with the log message, taking a new line
  // for each time it wraps the window or when there is a newline in the
  // message.
  std::string text = msg.text;
  size_t start = 0;
  int prefix_length = 0;
  for (int i = 0; i < 3; i++) {
    prefix_length += column_widths_[i] + 1;
  }
  for (;;) {
    std::string segment = text.substr(start);
    // Look for newlines in the segment and split there.
    size_t newline = segment.find('\n');
    if (newline != std::string::npos) {
      segment = segment.substr(0, newline);
    }
    if (segment.size() > column_widths_[3]) {
      segment = segment.substr(0, column_widths_[3]);
      // Move back to the first space to avoid splitting words.
      ssize_t i = segment.size() - 1;
      while (i > 0) {
        if (isspace(segment[i])) {
          break;
        }
        i--;
      }
      // If there is no space we just split the word.
      if (i != 0) {
        segment = segment.substr(0, i);
      }
    }
    lines.fields.push_back({.row = lines.num_rows,
                            .col = prefix_length,
                            .color = colors_[3],
                            .data = segment});
    lines.num_rows++;
    start += segment.size();

    // Skip newlines at the end of the segment.
    while (start < text.size() && text[start] == '\n') {
      start++;
    }
    // Skip spaces for continuation line.
    while (start < text.size() && isspace(text[start])) {
      start++;
    }
    if (start >= text.size()) {
      break;
    }
  }

  return lines;
}

} // namespace adastra::fido
