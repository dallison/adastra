// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "fido/alarms.h"
#include "retro/app.h"
#include "retro/dialog.h"
#include "fido/event_mux.h"
#include "fido/log_window.h"
#include "fido/processes.h"
#include "retro/screen.h"
#include "fido/subspace_stats.h"
#include "fido/subsystems.h"

#include "absl/container/flat_hash_set.h"
#include "coroutine.h"
#include "toolbelt/sockets.h"

#include <memory>

namespace fido {

class HelpWindow : public retro::InfoDialog {
public:
  HelpWindow(retro::Screen *screen, const std::string &ok)
      : retro::InfoDialog(screen,
                   retro::WindowOptions{.title = "Help",
                                 .nlines = 12,
                                 .ncols = 40,
                                 .x = screen->Width() / 2 - 20,
                                 .y = screen->Height() / 2 - 6},
                   ok) {}

  void WaitForUser(co::Coroutine *c);
};

class FDOApplication : public retro::Application {
public:
  FDOApplication(const toolbelt::InetAddress &flight_addr,
                 const std::string &subspace_socket);
  ~FDOApplication() = default;

  absl::Status Init() override;

  void Pause() override;
  void Resume() override;

private:
  void UserInputCoroutine(co::Coroutine *c);
  void Help(co::Coroutine *c);
  std::string GetFilter(const std::string &prompt, co::Coroutine *c);
  int GetLogLevel(co::Coroutine *c);

  std::string subspace_socket_;

  toolbelt::InetAddress flight_addr_;
  EventMux event_mux_;
  std::unique_ptr<SubsystemsWindow> subsystems_;
  std::unique_ptr<ProcessesWindow> processes_;
  std::unique_ptr<SubspaceStatsWindow> subspace_stats_;
  std::unique_ptr<AlarmsWindow> alarms_;
  std::unique_ptr<LogWindow> log_;
  std::unique_ptr<retro::YesNoDialog> quit_dialog_;
};
} // namespace fido