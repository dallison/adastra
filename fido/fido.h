#pragma once

#include "fido/screen.h"
#include "fido/event_mux.h"
#include "fido/alarms.h"
#include "fido/subspace_stats.h"
#include "fido/subsystems.h"
#include "fido/processes.h"
#include "fido/log_window.h"

#include "coroutine.h"
#include "absl/container/flat_hash_set.h"
#include "toolbelt/sockets.h"

#include <memory>

namespace fido {

class Application {
  public:
    Application(const toolbelt::InetAddress& flight_addr, const std::string& subspace_socket);
~Application() = default;

    void Run();

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) {
    coroutines_.insert(std::move(c));
  }

  co::CoroutineScheduler& Scheduler() {
    return scheduler_;
  }
  
private:
  void UserInputCoroutine(co::Coroutine* c);

  std::string subspace_socket_;
  Screen screen_;
  bool running_;
  co::CoroutineScheduler scheduler_;
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> coroutines_;

  toolbelt::InetAddress flight_addr_;
  EventMux event_mux_;
  std::unique_ptr<SubsystemsWindow> subsystems_;
  std::unique_ptr<ProcessesWindow> processes_;
  std::unique_ptr<SubspaceStatsWindow> subspace_stats_;
  std::unique_ptr<AlarmsWindow> alarms_;
  std::unique_ptr<LogWindow> log_;
};
}