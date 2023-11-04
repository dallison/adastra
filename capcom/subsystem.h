#pragma once

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "capcom/bitset.h"
#include "common/states.h"
#include "proto/capcom.pb.h"
#include "stagezero/client/client.h"
#include "toolbelt/fd.h"
#include "toolbelt/logging.h"
#include "toolbelt/triggerfd.h"
#include "common/vars.h"
#include "common/alarm.h"

namespace stagezero::capcom {

class Capcom;
class Subsystem;

// Messages are sent through the message pipe.
struct Message {
  enum Code {
    kChangeAdmin,
    kReportOper,
  };
  Code code;
  Subsystem *sender;
  uint32_t client_id;
  union {
    AdminState admin;
    OperState oper;
  } state;
};

class Process {
public:
  Process(std::string name) : name_(name) {}
  virtual ~Process() = default;
  virtual absl::Status Launch(stagezero::Client &client, co::Coroutine *c) = 0;
  absl::Status Stop(stagezero::Client &client, co::Coroutine *c);

  const std::string &Name() const { return name_; }

  void SetRunning() { running_ = true; }
  void SetStopped() { running_ = false; }
  bool IsRunning() const { return running_; }

  const std::string &GetProcessId() const { return process_id_; }

  int GetPid() const { return pid_; }

  void RaiseAlarm(Capcom& capcom, const Alarm& alarm);
  void ClearAlarm(Capcom& capcom);

protected:
  void ParseOptions(const stagezero::config::ProcessOptions &options);

  std::string name_;
  std::string description_;
  std::vector<Variable> vars_;
  std::vector<std::string> args_;
  int32_t startup_timeout_secs_;
  int32_t sigint_shutdown_timeout_secs_;
  int32_t sigterm_shutdown_timeout_secs_;
  bool notify_;

  bool running_ = false;
  std::string process_id_;
  int pid_;
  Alarm alarm_;
  bool alarm_raised_ = false;
};

class StaticProcess : public Process {
public:
  StaticProcess(std::string name, std::string executable,
                const stagezero::config::ProcessOptions &options);
  absl::Status Launch(stagezero::Client &client, co::Coroutine *c) override;

private:
  std::string executable_;
};

class Zygote : public StaticProcess {
public:
  Zygote(std::string name, std::string executable,
         stagezero::config::ProcessOptions &options)
      : StaticProcess(name, executable, options) {}
};

class VirtualProcess : public Process {
public:
  VirtualProcess(std::string name) : Process(name) {}
  absl::Status Launch(stagezero::Client &client, co::Coroutine *c) override;
};

class Subsystem : public std::enable_shared_from_this<Subsystem> {
public:
  Subsystem(std::string name, Capcom &capcom);
  ~Subsystem() {
    std::cerr << "Subsystem " << Name() << " destructed" << std::endl;
  }

  absl::Status
  AddStaticProcess(const stagezero::config::StaticProcess &proc,
                   const stagezero::config::ProcessOptions &options);

  void RemoveProcess(const std::string &name);

  void Run();
  void Stop();

  absl::Status SendMessage(const Message &message) const;

  const std::string &Name() const { return name_; }

  absl::Status Remove(bool recursive);

  bool CheckRemove(bool recursive);

  void BuildStatusEvent(capcom::proto::SubsystemStatus *event);

  void AddChild(std::shared_ptr<Subsystem> child) {
    std::cerr << "ADDING CHILD " << child->Name() << " to " << Name()
              << std::endl;
    children_.push_back(child);
  }

  void AddParent(std::shared_ptr<Subsystem> parent) {
    parents_.push_back(parent);
  }

private:
  enum class EventSource {
    kUnknown,
    kStageZero, // StageZero (process state or data).
    kMessage,   // Message from parents, children or API.
  };

  static constexpr int kDefaultMaxRestarts = 3;

  absl::Status BuildMessagePipe();
  absl::StatusOr<Message> ReadMessage() const;

  // TODO: remove parent function.

  void AddProcess(std::unique_ptr<Process> p) {
    processes_.push_back(std::move(p));
  }

  void RecordProcessId(const std::string &id, Process *p) {
    process_map_[id] = p;
  }

  void DeleteProcessId(const std::string &id) { process_map_.erase(id); }

  Process *FindProcess(const std::string &id) {
    auto it = process_map_.find(id);
    if (it == process_map_.end()) {
      return nullptr;
    }
    return it->second;
  }

  bool AllProcessesRunning() const {
    for (auto &p : processes_) {
      if (!p->IsRunning()) {
        return false;
      }
    }
    return true;
  }
  bool AllProcessesStopped() const {
    for (auto &p : processes_) {
      if (p->IsRunning()) {
        return false;
      }
    }
    return true;
  }
  // State event processing coroutines.
  // General event processor.  Calls handler for incoming events
  // passing the file descriptor upon which the event arrived.  If it
  // returns false, the loop terminates.
  void ProcessEvents(co::Coroutine *c,
                     std::function<bool(EventSource, co::Coroutine *)> handler);

  void Offline(uint32_t client_id, co::Coroutine *c);
  void StartingChildren(uint32_t client_id, co::Coroutine *c);
  void StartingProcesses(uint32_t client_id, co::Coroutine *c);
  void Online(uint32_t client_id, co::Coroutine *c);
  void StoppingProcesses(uint32_t client_id, co::Coroutine *c);
  void StoppingChildren(uint32_t client_id, co::Coroutine *c);
  void Restarting(uint32_t client_id, co::Coroutine *c);
  void Broken(uint32_t client_id, co::Coroutine *c);

  toolbelt::Logger &GetLogger() const;

  void EnterState(
      std::function<void(std::shared_ptr<Subsystem>, uint32_t, co::Coroutine *)>
          func,
      uint32_t client_id);

  void LaunchProcesses(co::Coroutine *c);
  void StopProcesses(co::Coroutine *c);

  void RestartIfPossible(std::string process_id, uint32_t client_id);

  void RestartNow(uint32_t client_id);
  void NotifyParents();
  void SendToChildren(AdminState state, uint32_t client_id);

  void RaiseAlarm(const Alarm& alarm);
  void ClearAlarm();

  co::CoroutineScheduler &Scheduler();

  std::string name_;
  Capcom &capcom_;
  bool running_ = false;
  AdminState admin_state_;
  OperState oper_state_;

  toolbelt::TriggerFd interrupt_;
  std::unique_ptr<stagezero::Client> stagezero_client_;

  // The command pipe is a pipe connected to this subsystem. The
  // incoming_command_ fd is the read end and command_ is the
  // write end.  Commands are send to the write end and we
  // receive them through incoming_command_.
  toolbelt::FileDescriptor incoming_message_;
  toolbelt::FileDescriptor message_;

  std::vector<std::unique_ptr<Process>> processes_;
  absl::flat_hash_map<std::string, Process *> process_map_;

  std::list<std::shared_ptr<Subsystem>> children_;
  std::list<std::shared_ptr<Subsystem>> parents_;

  BitSet active_clients_;

  int num_restarts_ = 0;
  int max_restarts_ = kDefaultMaxRestarts;

  Alarm alarm_;
  bool alarm_raised_ = false;
};

} // namespace stagezero::capcom