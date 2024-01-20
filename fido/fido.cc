#include "fido/fido.h"
#include "fido/event_mux.h"
#include "fido/screen.h"
#include "fido/table.h"

#include <stdlib.h>
#include <unistd.h>

namespace fido {

Application::Application(const toolbelt::InetAddress &flight_addr,
                         const std::string &subspace_socket)
    : subspace_socket_(subspace_socket), screen_(*this),
      flight_addr_(flight_addr), event_mux_(*this, flight_addr_) {}

void Application::Run() {
  if (absl::Status status = event_mux_.Init(); !status.ok()) {
    std::cerr << status << std::endl;
    return;
  }

  screen_.Open();
  if (screen_.Width() < 166 || screen_.Height() < 50) {
    screen_.Close();
    std::cerr << "You're gonna need a bigger boat, er, window.  At least 166x50\n";
    return;
  }
  screen_.PrintInMiddle(0, "FDO Console", kColorPairCyan);
  subsystems_ = std::make_unique<SubsystemsWindow>(&screen_, event_mux_);
  processes_ = std::make_unique<ProcessesWindow>(&screen_, event_mux_);
  subspace_stats_ = std::make_unique<SubspaceStatsWindow>(&screen_, subspace_socket_);
  alarms_ = std::make_unique<AlarmsWindow>(&screen_, event_mux_);
  log_ = std::make_unique<LogWindow>(&screen_, event_mux_);

  running_ = true;

  subsystems_->Run();
  processes_->Run();
  subspace_stats_->Run();
  alarms_->Run();
  log_->Run();

  AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_, [this](co::Coroutine *c) { UserInputCoroutine(c); }));

  scheduler_.Run();
}

void Application::UserInputCoroutine(co::Coroutine *c) {
  while (running_) {
    c->Wait(STDIN_FILENO, POLLIN);
    // Deal with user input here
  }
}

} // namespace fido
