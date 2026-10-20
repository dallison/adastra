// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/namespace.h"
#include "common/parameters.h"
#include "common/stream.h"
#include "coroutine.h"
#include "proto/config.pb.h"
#include "proto/control.pb.h"
#include "proto/parameters.pb.h"
#include "proto/telemetry.pb.h"
#include "stagezero/symbols.h"
#include "toolbelt/fd.h"
#include "toolbelt/pipe.h"
#include "toolbelt/sockets.h"
#include <chrono>
#include <memory>
#include <optional>
#include <signal.h>
#include <string>

// Change this to 0 if pidfd (on Linux only) is not available
#define HAVE_PIDFD 1

namespace adastra::stagezero {

class ClientHandler;
class StageZero;

struct StreamInfo {
  proto::StreamControl::Direction direction;
  proto::StreamControl::Disposition disposition;
  toolbelt::Pipe pipe;
  int fd; // Process fd to map to.
  std::string term_name;
  bool tty;
  std::string filename; // Filename for read/write.
};

class VirtualProcess;

// Ownership of processes is complex due to the fact that the
// process will run coroutines.  We store shared_ptrs to the
// Processes in the StageZero object and also capture the
// shared_ptrs in the coroutines.  Thus the Process instance
// will be deleted when the Process has been removed from the
// StageZero map and all coroutines have finished running.
class Process : public std::enable_shared_from_this<Process> {
public:
  Process(co::CoroutineScheduler &scheduler, StageZero &stagezero,
          std::shared_ptr<ClientHandler> client, std::string name);
  virtual ~Process() {}

  virtual absl::Status Start(co::Coroutine *c) = 0;
  virtual absl::Status Stop(co::Coroutine *c);
  void KillNow();

  virtual bool IsZygote() const { return false; }
  virtual bool IsVirtual() const { return false; }
  absl::Status SendInput(int fd, const std::string &data, co::Coroutine *c);
  absl::Status SendTelemetryCommand(const adastra::proto::telemetry::Command &cmd,
                                    co::Coroutine *c);

  absl::Status CloseFileDescriptor(int fd);

  const std::string &GetId() const { return process_id_; }

  const std::string &Name() const { return name_; }

  int WaitLoop(co::Coroutine *c, std::optional<std::chrono::seconds> timeout);

  bool IsStopping() const { return stopping_; }
  bool IsRunning() const { return running_; }
  int GetPid() const { return pid_; }
#ifdef __linux__
  toolbelt::FileDescriptor &GetPidFd() { return pid_fd_; }
  void SetPidFd(toolbelt::FileDescriptor pidfd) { pid_fd_ = std::move(pidfd); }
#endif
  void SetPid(int pid) { pid_ = pid; }
  void SetProcessId();
  virtual bool WillNotify() const = 0;
  virtual bool UseTelemetry() const = 0;
  virtual int StartupTimeoutSecs() const = 0;
  bool IsCritical() const { return critical_; }
  void SetDetached(bool detached) { detached_ = detached; }

  virtual void Notify(int64_t status) {}

  const std::shared_ptr<StreamInfo> FindNotifyStream() const;
  const std::shared_ptr<StreamInfo> FindParametersStream(bool read) const;
  const std::shared_ptr<StreamInfo> FindTelemetryStream(bool read) const;

  const std::vector<std::shared_ptr<StreamInfo>> &GetStreams() const {
    return streams_;
  }

  void SetSignalTimeouts(int sigint, int sigterm, int telemetry) {
    sigint_timeout_secs_ = sigint;
    sigterm_timeout_secs_ = sigterm;
    telemetry_shutdown_secs_ = telemetry;
  }

  int SigIntTimeoutSecs() const { return sigint_timeout_secs_; }
  int SigTermTimeoutSecs() const { return sigterm_timeout_secs_; }
  int TelemetryShutdownTimeoutSecs() const { return telemetry_shutdown_secs_; }

  void SetUserAndGroup(const std::string &user, const std::string &group) {
    user_ = user;
    group_ = group;
  }

  void SetCgroup(const std::string &cgroup) { cgroup_ = cgroup; }

  absl::Status AddToCgroup(int pid);

  bool IsDetached() const { return detached_; }

  StageZero &GetStageZero() { return stagezero_; }

  void SetNamespace(Namespace ns) { ns_ = std::move(ns); }

  void RunParameterServer();
  void RunTelemetryServer();

  absl::Status SendTelemetryShutdown(int exit_code, int timeout_secs, co::Coroutine *c);

  parameters::ParameterServer &Parameters() { return local_parameters_; }

  void SetWantsParameterEvents(bool wants) { wants_parameter_events_ = wants; }

  void SendParameterUpdateEvent(const std::string &name, const parameters::Value &value, co::Coroutine* c);
  void SendParameterDeleteEvent(const std::string &name, co::Coroutine* c);
  
protected:
  virtual int Wait() = 0;
  absl::Status BuildStreams(
      const google::protobuf::RepeatedPtrField<proto::StreamControl> &streams,
      bool notify, bool telemetry);

  static int SafeKill(int pid, int sig) {
    if (pid > 0) {
      return kill(pid, sig);
    }
    return -1;
  }
  void SendParameterEvent(const adastra::proto::parameters::ParameterEvent &event, co::Coroutine* c);

  co::CoroutineScheduler &scheduler_;
  StageZero &stagezero_;
  std::shared_ptr<ClientHandler> client_;
  std::string name_;
  int pid_ = 0;
#ifdef __linux__
  toolbelt::FileDescriptor pid_fd_;
#endif
  bool running_ = true;
  bool stopping_ = false;
  bool critical_ = false;
  std::string process_id_;
  std::vector<std::shared_ptr<StreamInfo>> streams_;
  SymbolTable local_symbols_;
  parameters::ParameterServer local_parameters_;
  int telemetry_shutdown_secs_ = 0;
  int sigint_timeout_secs_ = 0;
  int sigterm_timeout_secs_ = 0;
  std::string user_;
  std::string group_;
  bool interactive_ = false;
  Terminal interactive_terminal_;
  toolbelt::FileDescriptor interactive_this_end_;
  toolbelt::FileDescriptor interactive_proc_end_;
  std::string cgroup_;
  bool detached_ = false; // Process is detached from a client.
  std::optional<Namespace> ns_;
  bool wants_parameter_events_ = false;
  toolbelt::FileDescriptor parameters_event_read_fd_;
  toolbelt::FileDescriptor parameters_event_write_fd_;
};

class StaticProcess : public Process {
public:
  StaticProcess(co::CoroutineScheduler &scheduler, StageZero &stagezero,
                std::shared_ptr<ClientHandler> client,
                const stagezero::control::LaunchStaticProcessRequest &&req);

  absl::Status Start(co::Coroutine *c) override;
  bool WillNotify() const override { return req_.opts().notify(); }
  bool UseTelemetry() const override { return req_.opts().telemetry(); }

  int StartupTimeoutSecs() const override {
    return req_.opts().startup_timeout_secs();
  }

protected:
  absl::Status StartInternal(const std::vector<std::string> extra_env_vars,
                             bool send_start_event);
  absl::Status ForkAndExec(const std::vector<std::string> extra_env_vars);
  int Wait() override;
  stagezero::control::LaunchStaticProcessRequest req_;
};

class Zygote : public StaticProcess {
public:
  Zygote(co::CoroutineScheduler &scheduler, StageZero &stagezero,
         std::shared_ptr<ClientHandler> client,
         const stagezero::control::LaunchStaticProcessRequest &&req)
      : StaticProcess(scheduler, stagezero, client, std::move(req)) {}

  absl::Status Start(co::Coroutine *c) override;

  // Note that this is a completely blocking function that doesn't use
  // a coroutine.  It is called from other coroutines but since it talks
  // to a process through a socket, we can't interleave the messages
  // if multiple spawns happen at the same time.
  // Returns a pair of pid and pidfd (on linux only, all others will be 0);
  absl::StatusOr<std::pair<int, toolbelt::FileDescriptor>>
  Spawn(const stagezero::control::LaunchVirtualProcessRequest &req,
        const std::vector<std::shared_ptr<StreamInfo>> &streams);

  bool IsZygote() const override { return true; }

  void SetControlSocket(toolbelt::UnixSocket s) {
    control_socket_ = std::move(s);
  }

  void AddVirtualProcess(std::shared_ptr<VirtualProcess> p) {
    virtual_processes_.insert(p);
  }

  void RemoveVirtualProcess(std::shared_ptr<VirtualProcess> p) {
    virtual_processes_.erase(p);
  }

  void ForeachVirtualProcess(
      std::function<void(std::shared_ptr<VirtualProcess>)> fn) {
    for (auto &v : virtual_processes_) {
      fn(v);
    }
  }

private:
  std::pair<std::string, int> BuildZygoteSocketName();

  toolbelt::UnixSocket control_socket_;
  absl::flat_hash_set<std::shared_ptr<VirtualProcess>> virtual_processes_;
};

class VirtualProcess : public Process {
public:
  VirtualProcess(co::CoroutineScheduler &scheduler, StageZero &stagezero,
                 std::shared_ptr<ClientHandler> client,
                 const stagezero::control::LaunchVirtualProcessRequest &&req);

  absl::Status Start(co::Coroutine *c) override;
  bool WillNotify() const override { return req_.opts().notify(); }
  bool UseTelemetry() const override { return req_.opts().telemetry(); }
  int StartupTimeoutSecs() const override {
    return req_.opts().startup_timeout_secs();
  }

  void CloseNotifyPipe() { notify_pipe_.WriteFd().Reset(); }

  void Notify(int64_t status) override {
    (void)write(notify_pipe_.WriteFd().Fd(), &status, 8);
  }

  bool IsVirtual() const override { return true; }

private:
  int Wait() override;
  int WaitForZygoteNotification(co::Coroutine *c);

  stagezero::control::LaunchVirtualProcessRequest req_;
  toolbelt::Pipe notify_pipe_;
  std::shared_ptr<Zygote> zygote_;
};

} // namespace adastra::stagezero
