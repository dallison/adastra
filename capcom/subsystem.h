#pragma once

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "stagezero/client/client.h"
#include "toolbelt/fd.h"
#include "toolbelt/triggerfd.h"
#include "stagezero/proto/capcom.pb.h"

namespace stagezero::capcom {

class Capcom;
class Subsystem;

enum class AdminState {
  kOffline,
  kOnline,
};

enum class OperState {
  kOffline,
  kStartingChildren,
  kStartingProcesses,
  kOnline,
  kStoppingProcesses,
  kStoppingChildren,
  // TODO: restart.
};

struct Variable {
  std::string name;
  std::string value;
  bool exported = false;
};

// Messages are sent through the message pipe.
struct Message {
  enum Code {
    kChangeAdmin,
    kReportOper,
  };
  Code code;
  Subsystem *sender;
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

  const std::string &Name() const { return name_; }

  void SetRunning() { running_ = true; }
  void SetStopped() { running_ = false; }
  bool IsRunning() const { return running_; }

  const std::string &GetProcessId() const { return process_id_; }

  int GetPid() const { return pid_; }

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
  void AddChild(Subsystem *child);

  absl::Status
  AddStaticProcess(const stagezero::config::StaticProcess &proc,
                   const stagezero::config::ProcessOptions &options);

  void RemoveProcess(const std::string &name);

  void Run();
  void Stop();

  absl::Status SendMessage(const Message &message) const;

  const std::string &Name() const { return name_; }

  absl::Status Remove(bool recursive);

  void BuildStatusEvent(capcom::proto::SubsystemStatus* event);

private:
  enum class EventSource {
    kUnknown,
    kStageZero,       // StageZero (process state or data).
    kMessage,         // Message from parents, children or API.
  };

  absl::Status BuildMessagePipe();
  absl::StatusOr<Message> ReadMessage() const;

  void AddParent(Subsystem *parent, int event_fd, int command_fd);
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

  // State event processing coroutines.
  // General event processor.  Calls handler for incoming events
  // passing the file descriptor upon which the event arrived.  If it
  // returns false, the loop terminates.
  void ProcessEvents(co::Coroutine *c, std::function<bool(EventSource, co::Coroutine*)> handler);

  void Offline(co::Coroutine *c);
  void StartingProcesses(co::Coroutine *c);
  void Online(co::Coroutine *c);
  void StoppingProcesses(co::Coroutine *c);

  void EnterState(
      std::function<void(std::shared_ptr<Subsystem>, co::Coroutine *)> func);

  void LaunchProcesses(co::Coroutine *c);
  void StopProcesses(co::Coroutine *c);

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
};

} // namespace stagezero::capcom