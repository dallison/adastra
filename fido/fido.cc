// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "fido/fido.h"
#include "fido/event_mux.h"
#include "retro/screen.h"
#include "retro/table.h"

#include <stdlib.h>
#include <unistd.h>

namespace adastra::fido {

void HelpWindow::WaitForUser(co::Coroutine *c) {
  return InfoDialog::WaitForUser(
      {"1 - filter subsystems", "2 - filter processes",
       "3 - filter subspace channels", "4 - filter alarms", "5 - filter log",
       "l - set log level", "q - quit", "? - help"},
      c);
}

FDOApplication::FDOApplication(const toolbelt::InetAddress &flight_addr,
                               const std::string &subspace_socket)
    : Application(50, 166), subspace_socket_(subspace_socket),
      flight_addr_(flight_addr), event_mux_(*this, flight_addr_) {}

absl::Status FDOApplication::Init() {
  screen_.PrintInMiddle(0, "FDO Console", retro::kColorPairCyan);

  std::string help_text = "Hit ? for help";
  screen_.PrintAt(0, screen_.Width() - help_text.size(), help_text,
                  retro::kColorPairMagenta);

  event_mux_.Init();

  subsystems_ = std::make_unique<SubsystemsWindow>(&screen_, event_mux_);
  processes_ = std::make_unique<ProcessesWindow>(&screen_, event_mux_);
  subspace_stats_ =
      std::make_unique<SubspaceStatsWindow>(&screen_, subspace_socket_);
  alarms_ = std::make_unique<AlarmsWindow>(&screen_, event_mux_);
  log_ = std::make_unique<LogWindow>(&screen_, event_mux_);
  quit_dialog_ = std::make_unique<retro::YesNoDialog>(
      &screen_,
      retro::WindowOptions{.title = "Quit",
                           .nlines = 7,
                           .ncols = 30,
                           .y = screen_.Height() / 2 - 3,
                           .x = screen_.Width() / 2 - 15},
      "Yes", "No");

  
  subsystems_->Run();
  processes_->Run();
  subspace_stats_->Run();
  alarms_->Run();
  log_->Run();

  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_, [this](co::Coroutine *c) { UserInputCoroutine(c); }));

  return absl::OkStatus();
}


void FDOApplication::UserInputCoroutine(co::Coroutine *c) {
  std::string filter;
  for (;;) {
    c->Wait(STDIN_FILENO, POLLIN);
    // Deal with user input here
    int ch = getch();
    switch (ch) {
    case 'q':
      Pause();
      if (quit_dialog_->GetUserInput("Quit Program?", c)) {
        endwin();
        exit(0);
      }
      Resume();
      refresh();
      break;

    case '?':
      Help(c);
      break;

    case '1':
      filter = GetFilter("Filter for subsystems", c);
      subsystems_->SetFilter(filter);
      subsystems_->ApplyFilter();
      break;

    case '2':
      filter = GetFilter("Filter for processes", c);
      processes_->SetFilter(filter);
      processes_->ApplyFilter();
      break;

    case '3':
      filter = GetFilter("Filter for subspace channels", c);
      subspace_stats_->SetFilter(filter);
      subspace_stats_->ApplyFilter();
      break;

    case '4':
      filter = GetFilter("Filter for alarms", c);
      alarms_->SetFilter(filter);
      alarms_->ApplyFilter();
      break;

    case '5':
      filter = GetFilter("Filter for logs", c);
      log_->SetFilter(filter);
      log_->ApplyFilter();
      break;

    case 'l': {
      int level = GetLogLevel(c);
      if (level < 0 || level > 4) {
        break;
      }
      toolbelt::LogLevel log_level;
      switch (level) {
      case 0:
        log_level = toolbelt::LogLevel::kVerboseDebug;
        break;
      case 1:
        log_level = toolbelt::LogLevel::kDebug;
        break;
      case 2:
        log_level = toolbelt::LogLevel::kInfo;
        break;
      case 3:
        log_level = toolbelt::LogLevel::kWarning;
        break;
      case 4:
        log_level = toolbelt::LogLevel::kError;
        break;
      }
      log_->SetLogLevel(log_level);
      log_->ApplyFilter();
      break;
    }
    }
  }
}

void FDOApplication::Help(co::Coroutine *c) {
  Pause();
  HelpWindow help(&screen_, "OK");
  help.WaitForUser(c);
  Resume();
  refresh();
}

std::string FDOApplication::GetFilter(const std::string &prompt,
                                      co::Coroutine *c) {
  Pause();
  retro::UserInputDialog input(&screen_,
                               {.title = "Enter Filter",
                                .nlines = 7,
                                .ncols = 40,
                                .y = screen_.Height() / 2 - 3,
                                .x = screen_.Width() / 2 - 20},
                               "OK");
  std::string filter = input.GetUserInput(prompt, c);
  Resume();
  refresh();
  return filter;
}

int FDOApplication::GetLogLevel(co::Coroutine *c) {
  Pause();
  retro::SelectionDialog selection(&screen_,
                                   {.title = "Enter Log level",
                                    .nlines = 12,
                                    .ncols = 40,
                                    .y = screen_.Height() / 2 - 6,
                                    .x = screen_.Width() / 2 - 20},
                                   "OK", "Cancel");

  int level = selection.GetSelection(
      {"verbose-debug", "debug", "info", "warning", "error"}, c);
  Resume();
  refresh();
  return level;
}

void FDOApplication::Pause() {
  subsystems_->Pause();
  processes_->Pause();
  subspace_stats_->Pause();
  alarms_->Pause();
  log_->Pause();
}

void FDOApplication::Resume() {
  subsystems_->Resume();
  processes_->Resume();
  subspace_stats_->Resume();
  alarms_->Resume();
  log_->Resume();

  subsystems_->Draw();
  processes_->Draw();
  subspace_stats_->Draw();
  alarms_->Draw();
  log_->Draw();
}

} // namespace adastra::fido
