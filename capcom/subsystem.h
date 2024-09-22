// Copyright 2024 David Allison
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
#include "toolbelt/pipe.h"
#include "toolbelt/triggerfd.h"

namespace adastra::capcom {

using namespace std::chrono_literals;

class Capcom;
class Subsystem;
struct Compute;
class Process;

constexpr uint32_t kNoClient = -1U;

// Messages are sent through the message pipe.
struct Message {
  enum Code {
    kChangeAdmin,
    kReportOper,
    kAbort, // See emergency_abort.
    kRestart,
    kRestartProcesses,
    kRestartCrashedProcesses,
  };
  Code code;
  Subsystem *sender;
  uint32_t client_id;
  union {
    AdminState admin;
    OperState oper;
  } state;

  // An emergency abort is used to bring the whole system down.
  bool emergency_abort;

  // Information for interactive.
  bool interactive;
  int output_fd;
  int cols;
  int rows;
  std::string term_name;

  // For Code::kRestartProcesses, these processes will be restarted.  If empty,
  // all processes will be restarted.
  std::vector<std::shared_ptr<Process>> processes;
};

class Process {
public:
  Process(Capcom &capcom, std::string name, std::string compute,
          std::shared_ptr<stagezero::Client> client)
      : capcom_(capcom), name_(name), compute_(compute), client_(client) {}
  virtual ~Process() = default;
  virtual absl::Status Launch(Subsystem *subsystem, co::Coroutine *c) = 0;
  absl::Status Stop(co::Coroutine *c);

  const std::string &Name() const { return name_; }

  void SetRunning() { running_ = true; }
  void SetStopped() { running_ = false; }
  bool IsRunning() const { return running_; }

  const std::string &GetProcessId() const { return process_id_; }
  const std::string &Compute() const { return compute_; }

  int GetPid() const { return pid_; }

  void RaiseAlarm(Capcom &capcom, const Alarm &alarm);
  void ClearAlarm(Capcom &capcom);

  const Alarm *GetAlarm() const {
    if (alarm_raised_) {
      return &alarm_;
    }
    return nullptr;
  }

  void ResetAlarmCount() { alarm_count_ = 0; }

  virtual bool IsZygote() const { return false; }
  virtual bool IsVirtual() const { return false; }

  absl::Status SendInput(int fd, const std::string &data, co::Coroutine *c);

  absl::Status CloseFd(int fd, co::Coroutine *c);

  bool IsCritical() const { return critical_; }
  bool IsOneShot() const { return oneshot_; }

  int AlarmCount() const { return alarm_count_; }

  int NumRestarts() const { return num_restarts_; }

  void IncNumRestarts() { num_restarts_++; }

  void ResetNumRestarts() { num_restarts_ = 0; }

  void SetMaxRestarts(int max_restarts) { max_restarts_ = max_restarts; }

  int MaxRestarts() const { return max_restarts_; }

  int ExitStatus() const { return exit_status_; }
  int Exited() const { return exited_; }

  void SetExit(bool exited, int status) {
      exited_ = exited;
  exit_status_ = status;
  }

  std::chrono::seconds IncRestartDelay() {
    auto old_delay = restart_delay_;
    restart_delay_ *= 2;
    if (restart_delay_ > kMaxRestartDelay) {
      restart_delay_ = kMaxRestartDelay;
    }
    return old_delay;
  }

protected:
  void ParseOptions(const stagezero::config::ProcessOptions &options);
  void ParseStreams(
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams);

  Capcom &capcom_;
  std::string name_;
  std::string compute_;
  std::string description_;
  std::vector<Variable> vars_;
  std::vector<std::string> args_;
  int32_t startup_timeout_secs_;
  int32_t sigint_shutdown_timeout_secs_;
  int32_t sigterm_shutdown_timeout_secs_;
  bool notify_;
  int max_restarts_;

  bool running_ = false;
  std::string process_id_;
  int pid_;
  Alarm alarm_;
  bool alarm_raised_ = false;
  int alarm_count_ = 0;
  std::shared_ptr<stagezero::Client> client_;
  std::vector<Stream> streams_;
  bool interactive_ = false;
  std::string user_;
  std::string group_;
  bool critical_ = false;
  bool oneshot_ = false;
  std::string cgroup_ = "";
  int num_restarts_ = 0;
  int exited_ = 0;
  int exit_status_ = 0;

  static constexpr std::chrono::seconds kMaxRestartDelay = 32s;
  std::chrono::seconds restart_delay_ = 1s;
};

class StaticProcess : public Process {
public:
  StaticProcess(
      Capcom &capcom, std::string name, std::string compute,
      std::string executable, const stagezero::config::ProcessOptions &options,
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
      Capcom &capcom, std::string name, std::string compute,
      std::string executable, const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      std::shared_ptr<stagezero::Client> client)
      : StaticProcess(capcom, name, compute, executable, options, streams,
                      std::move(client)) {}
  absl::Status Launch(Subsystem *subsystem, co::Coroutine *c) override;
  bool IsZygote() const override { return true; }
};

class VirtualProcess : public Process {
public:
  VirtualProcess(
      Capcom &capcom, std::string name, std::string compute,
      std::string zygote_name, std::string dso, std::string main_func,
      const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      std::shared_ptr<stagezero::Client> client);
  absl::Status Launch(Subsystem *subsystem, co::Coroutine *c) override;
  bool IsVirtual() const override { return true; }

private:
  std::string zygote_name_;
  std::string dso_;
  std::string main_func_;
};

class Subsystem : public std::enable_shared_from_this<Subsystem> {
public:
  enum class RestartPolicy {
    kAutomatic,
    kManual,
    kProcessOnly, // Restart only the process that exited
  };

  Subsystem(std::string name, Capcom &capcom, std::vector<Variable> vars,
            std::vector<Stream> streams, int max_restarts, bool critical,
            RestartPolicy restart_policy);
  ~Subsystem() {}

  absl::Status AddStaticProcess(
      const stagezero::config::StaticProcess &proc,
      const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      const Compute *compute, int max_restarts, co::Coroutine *c);

  absl::Status AddZygote(
      const stagezero::config::StaticProcess &proc,
      const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      const Compute *compute, int max_restarts, co::Coroutine *c);

  absl::Status AddVirtualProcess(
      const stagezero::config::VirtualProcess &proc,
      const stagezero::config::ProcessOptions &options,
      const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
          &streams,
      const Compute *compute, int max_restarts, co::Coroutine *c);

  void RemoveProcess(const std::string &name);

  void Run();
  void Stop();

  absl::Status SendMessage(std::shared_ptr<Message> message);

  const std::string &Name() const { return name_; }

  absl::Status Remove(bool recursive);

  bool CheckRemove(bool recursive);

  void BuildStatus(adastra::proto::SubsystemStatus *status);

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

  const Terminal &InteractiveTerminal() const { return interactive_terminal_; }

  bool IsCritical() const { return critical_; }

  std::shared_ptr<Process> FindProcessName(const std::string &name) {
    auto it = process_map_.find(name);
    if (it == process_map_.end()) {
      return nullptr;
    }
    return it->second;
  }

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
  absl::StatusOr<std::shared_ptr<Message>> ReadMessage();

  // TODO: remove parent function.

  void AddProcess(std::shared_ptr<Process> p) {
    process_map_.insert(std::make_pair(p->Name(), p));
    processes_.push_back(std::move(p));
  }

  void RecordProcessId(const std::string &id, std::shared_ptr<Process> p) {
    process_id_map_[id] = p;
  }

  void DeleteProcessId(const std::string &id) { process_map_.erase(id); }

  std::shared_ptr<Process> FindProcess(const std::string &id) {
    auto it = process_id_map_.find(id);
    if (it == process_id_map_.end()) {
      return nullptr;
    }
    return it->second;
  }

  Zygote *FindZygote(const std::string &name);

  bool AllProcessesRunning() const {
    for (auto &p : processes_) {
      // Count a oneshot process as running only if it has exited successfully.
      if (p->IsOneShot()) {
        if (p->IsRunning()) {
          return false;
        }
        if (p->Exited() == 0 && p->ExitStatus() == 0) {
          // Onsshot process has exited successfully.
          continue;
        }
      }
      if (!p->IsRunning()) {
        return false;
      }
    }
    return true;
  }
  bool AllProcessesStopped(
      const std::vector<std::shared_ptr<Process>> &procs) const {
    for (auto &p : procs) {
      if (p->IsRunning()) {
        return false;
      }
    }
    return true;
  }

  bool AllProcessesStopped() const { return AllProcessesStopped(processes_); }

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
          handler,
      std::chrono::seconds timeout = 0s);

  void Offline(uint32_t client_id, co::Coroutine *c);
  void StartingChildren(uint32_t client_id, co::Coroutine *c);
  void StartingProcesses(uint32_t client_id, co::Coroutine *c);
  void Online(uint32_t client_id, co::Coroutine *c);
  void StoppingProcesses(uint32_t client_id, co::Coroutine *c);
  void StoppingChildren(uint32_t client_id, co::Coroutine *c);
  void Restarting(uint32_t client_id, co::Coroutine *c);
  void Broken(uint32_t client_id, co::Coroutine *c);
  void RestartingProcesses(uint32_t client_id, co::Coroutine *c);

  StateTransition Abort(bool emergency);

  void EnterState(OperState state, uint32_t client_id);

  absl::Status LaunchProcesses(co::Coroutine *c);
  void StopProcesses(co::Coroutine *c);
  void ResetProcessRestarts();

  StateTransition RestartIfPossibleAfterProcessCrash(std::string process_id,
                                                     uint32_t client_id,
                                                     bool exited,
                                                     int signal_or_status,
                                                     co::Coroutine *c);

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
  void RestartProcesses(const std::vector<std::shared_ptr<Process>> &processes,
                        co::Coroutine *c);
  void WaitForRestart(co::Coroutine *c);

  OperState HandleAdminCommand(const Message &message,
                               OperState next_state_no_active_clients,
                               OperState next_state_active_clients);

  co::CoroutineScheduler &Scheduler();

  void SendOutput(int fd, const std::string &data, co::Coroutine *c);

  std::shared_ptr<Process> FindInteractiveProcess();

  std::string name_;
  Capcom &capcom_;
  std::vector<Variable> vars_;
  std::vector<Stream> streams_;
  bool running_ = false;
  AdminState admin_state_ = AdminState::kOffline;
  OperState oper_state_ = OperState::kOffline;
  OperState prev_oper_state_ = OperState::kOffline;

  toolbelt::TriggerFd interrupt_;

  // The command pipe is a pipe connected to this subsystem.
  // Commands are send to the write end and we
  // receive them through the read end.
  toolbelt::SharedPtrPipe<Message> message_pipe_;

  std::vector<std::shared_ptr<Process>> processes_;
  absl::flat_hash_map<std::string, std::shared_ptr<Process>> process_map_;
  absl::flat_hash_map<std::string, std::shared_ptr<Process>> process_id_map_;

  std::list<std::shared_ptr<Subsystem>> children_;
  std::list<std::shared_ptr<Subsystem>> parents_;

  BitSet active_clients_;

  int num_restarts_ = 0;
  int max_restarts_ = kDefaultMaxRestarts;
  bool critical_ = false;
  int restart_count_ = 0;
  RestartPolicy restart_policy_ = RestartPolicy::kAutomatic;
  // When restarting processes, this holds the set of process we are restarting.
  std::vector<std::shared_ptr<Process>> processes_to_restart_;

  Alarm alarm_;
  bool alarm_raised_ = false;
  int alarm_count_ = 0;

  static constexpr std::chrono::duration<int> kMaxRestartDelay = 32s;
  std::chrono::duration<int> restart_delay_ = 1s;

  // Mapping of compute name vs StageZero client for that compute.
  // Each StageZero client is unique to this subsystem, so each subsystem
  // maintains its own connections to the StageZeros on the computes.
  absl::flat_hash_map<std::string, std::shared_ptr<stagezero::Client>>
      computes_;

  bool interactive_ = false;
  toolbelt::FileDescriptor interactive_output_;
  Terminal interactive_terminal_;
};

} // namespace adastra::capcom
