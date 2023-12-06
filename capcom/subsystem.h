// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <chrono>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "capcom/bitset.h"
#include "common/alarm.h"
#include "common/states.h"
#include "common/vars.h"
#include "proto/capcom.pb.h"
#include "proto/event.pb.h"
#include "proto/stream.pb.h"
#include "stagezero/client/client.h"
#include "toolbelt/fd.h"
#include "toolbelt/logging.h"
#include "toolbelt/triggerfd.h"

namespace stagezero::capcom {

using namespace std::chrono_literals;

class Capcom;
class Subsystem;
struct Compute;

constexpr uint32_t kNoClient = -1U;

// Messages are sent through the message pipe.
struct Message {
  enum Code {
    kChangeAdmin,
    kReportOper,
    kAbort,
  };
  Code code;
  Subsystem *sender;
  uint32_t client_id;
  union {
    AdminState admin;
    OperState oper;
  } state;

  // Information for interactive.
  bool interactive;
  int output_fd;
};

class Process {
public:
  Process(Capcom &capcom, std::string name,
          std::shared_ptr<stagezero::Client> client)
      : capcom_(capcom), name_(name), client_(client) {}
  virtual ~Process() = default;
  virtual absl::Status Launch(Subsystem *subsystem, co::Coroutine *c) = 0;
  absl::Status Stop(co::Coroutine *c);

  const std::string &Name() const { return name_; }

  void SetRunning() { running_ = true; }
  void SetStopped() { running_ = false; }
  bool IsRunning() const { return running_; }

  const std::string &GetProcessId() const { return process_id_; }

  int GetPid() const { return pid_; }

  void RaiseAlarm(Capcom &capcom, const Alarm &alarm);
  void ClearAlarm(Capcom &capcom);

  const Alarm *GetAlarm() const {
    if (alarm_raised_) {
      return &alarm_;
    }
    return nullptr;
  }

  virtual bool IsZygote() const { return false; }

  absl::Status SendInput(int fd, const std::string &data, co::Coroutine *c);

  absl::Status CloseFd(int fd, co::Coroutine *c);

protected:
  void ParseOptions(const stagezero::config::ProcessOptions &options);
  void ParseStreams(
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams);

  Capcom &capcom_;
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
  std::shared_ptr<stagezero::Client> client_;
  std::vector<Stream> streams_;
  bool interactive_ = false;
};

class StaticProcess : public Process {
public:
  StaticProcess(
      Capcom &capcom, std::string name, std::string executable,
      const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      std::shared_ptr<stagezero::Client> client);
  absl::Status Launch(Subsystem *subsystem, co::Coroutine *c) override;

protected:
  std::string executable_;
};

class Zygote : public StaticProcess {
public:
  Zygote(
      Capcom &capcom, std::string name, std::string executable,
      const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      std::shared_ptr<stagezero::Client> client)
      : StaticProcess(capcom, name, executable, options, streams,
                      std::move(client)) {}
  absl::Status Launch(Subsystem *subsystem, co::Coroutine *c) override;
  bool IsZygote() const override { return true; }
};

class VirtualProcess : public Process {
public:
  VirtualProcess(
      Capcom &capcom, std::string name, std::string zygote_name,
      std::string dso, std::string main_func,
      const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      std::shared_ptr<stagezero::Client> client);
  absl::Status Launch(Subsystem *subsystem, co::Coroutine *c) override;

private:
  std::string zygote_name_;
  std::string dso_;
  std::string main_func_;
};

class Subsystem : public std::enable_shared_from_this<Subsystem> {
public:
  Subsystem(std::string name, Capcom &capcom, std::vector<Variable> vars,
  std::vector<Stream> streams);
  ~Subsystem() {}

  absl::Status AddStaticProcess(
      const stagezero::config::StaticProcess &proc,
      const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      const Compute *compute, co::Coroutine *c);

  absl::Status AddZygote(
      const stagezero::config::StaticProcess &proc,
      const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      const Compute *compute, co::Coroutine *c);

  absl::Status AddVirtualProcess(
      const stagezero::config::VirtualProcess &proc,
      const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      const Compute *compute, co::Coroutine *c);

  void RemoveProcess(const std::string &name);

  void Run();
  void Stop();

  absl::Status SendMessage(const Message &message) const;

  const std::string &Name() const { return name_; }

  absl::Status Remove(bool recursive);

  bool CheckRemove(bool recursive);

  void BuildStatus(stagezero::proto::SubsystemStatus *status);

  void AddChild(std::shared_ptr<Subsystem> child) {
    children_.push_back(child);
  }

  void AddParent(std::shared_ptr<Subsystem> parent) {
    parents_.push_back(parent);
  }

  absl::Status RemoveChild(Subsystem *child) {
    for (auto it = children_.begin(); it != children_.end(); it++) {
      if (it->get() == child) {
        children_.erase(it);
        return absl::OkStatus();
      }
    }
    return absl::InternalError(absl::StrFormat(
        "Subsystem %s is not a child of %s", child->Name(), Name()));
  }

  absl::Status RemoveParent(Subsystem *parent) {
    for (auto it = parents_.begin(); it != parents_.end(); it++) {
      if (it->get() == parent) {
        parents_.erase(it);
        return absl::OkStatus();
      }
    }
    return absl::InternalError(absl::StrFormat(
        "Subsystem %s is not a parent of %s", parent->Name(), Name()));
  }

  void CollectAlarms(std::vector<Alarm> &alarms) const;

  bool IsOffline() const {
    return admin_state_ == AdminState::kOffline &&
           oper_state_ == OperState::kOffline;
  }

  absl::Status SendInput(const std::string &process, int fd,
                         const std::string &data, co::Coroutine *c);

  absl::Status CloseFd(const std::string &process, int fd, co::Coroutine *c);

  const std::vector<Variable> &Vars() const { return vars_; }
  const std::vector<Stream> &Streams() const { return streams_; }

private:
  enum class EventSource {
    kUnknown,
    kStageZero, // StageZero (process state or data).
    kMessage,   // Message from parents, children or API.
  };

  enum class StateTransition {
    kStay,
    kLeave,
  };

  static constexpr int kDefaultMaxRestarts = 3;

  static std::function<void(std::shared_ptr<Subsystem>, uint32_t,
                            co::Coroutine *)>
      state_funcs_[];

  absl::Status BuildMessagePipe();
  absl::StatusOr<Message> ReadMessage() const;

  // TODO: remove parent function.

  void AddProcess(std::unique_ptr<Process> p) {
    process_map_.insert(std::make_pair(p->Name(), p.get()));
    processes_.push_back(std::move(p));
  }

  void RecordProcessId(const std::string &id, Process *p) {
    process_id_map_[id] = p;
  }

  void DeleteProcessId(const std::string &id) { process_map_.erase(id); }

  Process *FindProcess(const std::string &id) {
    auto it = process_id_map_.find(id);
    if (it == process_id_map_.end()) {
      return nullptr;
    }
    return it->second;
  }

  Process *FindProcessName(const std::string &name) {
    auto it = process_map_.find(name);
    if (it == process_map_.end()) {
      return nullptr;
    }
    return it->second;
  }

  Zygote *FindZygote(const std::string &name);

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

  absl::StatusOr<std::shared_ptr<stagezero::Client>>
  ConnectToStageZero(const Compute *compute, co::Coroutine *c);

  // State event processing coroutines.
  // General event processor.  Calls handler for incoming events
  // passing the file descriptor upon which the event arrived.  If it
  // returns false, the loop terminates.
  void RunSubsystemInState(
      co::Coroutine *c,
      std::function<StateTransition(EventSource,
                                    std::shared_ptr<stagezero::Client> client,
                                    co::Coroutine *)>
          handler);

  void Offline(uint32_t client_id, co::Coroutine *c);
  void StartingChildren(uint32_t client_id, co::Coroutine *c);
  void StartingProcesses(uint32_t client_id, co::Coroutine *c);
  void Online(uint32_t client_id, co::Coroutine *c);
  void StoppingProcesses(uint32_t client_id, co::Coroutine *c);
  void StoppingChildren(uint32_t client_id, co::Coroutine *c);
  void Restarting(uint32_t client_id, co::Coroutine *c);
  void Broken(uint32_t client_id, co::Coroutine *c);

  void Abort();

  toolbelt::Logger &GetLogger() const;

  void EnterState(OperState state, uint32_t client_id);

  absl::Status LaunchProcesses(co::Coroutine *c);
  void StopProcesses(co::Coroutine *c);

  void RestartIfPossibleAfterProcessCrash(std::string process_id,
                                          uint32_t client_id, co::Coroutine *c);

  void RestartIfPossible(uint32_t client_id, co::Coroutine *c);

  void RestartNow(uint32_t client_id);
  void NotifyParents();
  void SendToChildren(AdminState state, uint32_t client_id);

  void RaiseAlarm(const Alarm &alarm);
  void ClearAlarm();

  void ResetResartState() {
    num_restarts_ = 0;
    restart_delay_ = 1s;
  }

  OperState HandleAdminCommand(const Message &message,
                               OperState next_state_no_active_clients,
                               OperState next_state_active_clients);

  co::CoroutineScheduler &Scheduler();

  void SendOutput(int fd, const std::string &data, co::Coroutine *c);

  Process *FindInteractiveProcess();

  std::string name_;
  Capcom &capcom_;
  std::vector<Variable> vars_;
  std::vector<Stream> streams_;
  bool running_ = false;
  AdminState admin_state_;
  OperState oper_state_;

  toolbelt::TriggerFd interrupt_;

  // The command pipe is a pipe connected to this subsystem. The
  // incoming_command_ fd is the read end and command_ is the
  // write end.  Commands are send to the write end and we
  // receive them through incoming_command_.
  toolbelt::FileDescriptor incoming_message_;
  toolbelt::FileDescriptor message_;

  std::vector<std::unique_ptr<Process>> processes_;
  absl::flat_hash_map<std::string, Process *> process_map_;
  absl::flat_hash_map<std::string, Process *> process_id_map_;

  std::list<std::shared_ptr<Subsystem>> children_;
  std::list<std::shared_ptr<Subsystem>> parents_;

  BitSet active_clients_;

  int num_restarts_ = 0;
  int max_restarts_ = kDefaultMaxRestarts;

  Alarm alarm_;
  bool alarm_raised_ = false;

  static constexpr std::chrono::duration<int> kMaxRestartDelay = 32s;
  std::chrono::duration<int> restart_delay_ = 1s;

  // Mapping of compute name vs StageZero client for that compute.
  // Each StageZero client is unique to this subsystem, so each subsystem
  // maintains its own connections to the StageZeros on the computes.
  absl::flat_hash_map<std::string, std::shared_ptr<stagezero::Client>>
      computes_;

  bool interactive_ = false;
  toolbelt::FileDescriptor interactive_output_;
};

} // namespace stagezero::capcom