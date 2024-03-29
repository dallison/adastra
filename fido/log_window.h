// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "common/event.h"
#include "common/log.h"
#include "fido/event_mux.h"
#include "retro/panel.h"

#include <list>

namespace adastra::fido {

class LogWindow : public retro::Panel {
public:
  LogWindow(retro::Screen *screen, EventMux &mux);
  ~LogWindow() = default;

  void Run() override;

  void Draw(bool refresh = true) override;

  void SetLogLevel(toolbelt::LogLevel level) { log_level_ = level; }

  void ApplyFilter() override;

private:
  struct Field {
    int row; // Relative row within line.
    int col;
    int color;
    std::string data;
  };

  struct MessageLines {
    int num_rows;
    std::vector<Field> fields;
  };

  void RunnerCoroutine(co::Coroutine *c);
  void Render();
  MessageLines RenderMessage(const adastra::LogMessage &msg);

  EventMux &mux_;
  toolbelt::SharedPtrPipe<adastra::Event> event_pipe_;
  std::list<adastra::LogMessage> logs_;

  static constexpr int kNumColumns = 4;
  size_t column_widths_[kNumColumns];
  int colors_[kNumColumns];
  toolbelt::LogLevel log_level_ = toolbelt::LogLevel::kInfo;
};

} // namespace adastra::fido
