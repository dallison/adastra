// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/stream.h"
#include "coroutine.h"
#include "proto/config.pb.h"
#include "proto/control.pb.h"
#include "stagezero/symbols.h"
#include "toolbelt/fd.h"
#include "toolbelt/pipe.h"
#include "toolbelt/sockets.h"
#include <memory>
#include <string>
#include <chrono>
#include <optional>
#include <signal.h>
#include "absl/container/flat_hash_set.h"

namespace adastra::stagezero {

class ClientHandler;

struct StreamInfo {
  proto::StreamControl::Direction direction;
  proto::StreamControl::Disposition disposition;
  toolbelt::Pipe pipe;
  int fd; // Process fd to map to.
  std::string term_name;
  bool tty;
  std::string filename;     // Filename for read/write.
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
  Process(co::CoroutineScheduler &scheduler,
          std::shared_ptr<ClientHandler> client, std::string name);
  virtual ~Process() {}

  virtual absl::Status Start(co::Coroutine *c) = 0;
  virtual absl::Status Stop(co::Coroutine *c);
  void KillNow();

  virtual bool IsZygote() const { return false; }
  virtual bool IsVirtual() const { return false; }
  absl::Status SendInput(int fd, const std::string &data, co::Coroutine *c);

  absl::Status CloseFileDescriptor(int fd);

  const std::string &GetId() const { return process_id_; }

  const std::string &Name() const { return name_; }

  int WaitLoop(co::Coroutine *c, std::optional<std::chrono::seconds> timeout);

  bool IsStopping() const { return stopping_; }
  bool IsRunning() const { return running_; }
  int GetPid() const { return pid_; }
  void SetPid(int pid) { pid_ = pid; }
  void SetProcessId();
  virtual bool WillNotify() const = 0;
  virtual int StartupTimeoutSecs() const = 0;
  bool IsCritical() const { return critical_; }

  virtual void Notify(int64_t status) {}

  const std::shared_ptr<StreamInfo> FindNotifyStream() const;

  const std::vector<std::shared_ptr<StreamInfo>> &GetStreams() const {
    return streams_;
  }

  void SetSignalTimeouts(int sigint, int sigterm) {
    sigint_timeout_secs_ = sigint;
    sigterm_timeout_secs_ = sigterm;
  }

  int SigIntTimeoutSecs() const { return sigint_timeout_secs_; }
  int SigTermTimeoutSecs() const { return sigterm_timeout_secs_; }

  void SetUserAndGroup(const std::string &user, const std::string &group) {
    user_ = user;
    group_ = group;
  }

protected:
  virtual int Wait() = 0;
  absl::Status BuildStreams(
      const google::protobuf::RepeatedPtrField<proto::StreamControl> &streams,
      bool notify);

  static int SafeKill(int pid, int sig) {
    if (pid > 0) {
      return kill(pid, sig);
    }
    return -1;
  }
  co::CoroutineScheduler &scheduler_;
  std::shared_ptr<ClientHandler> client_;
  std::string name_;
  int pid_ = 0;
  bool running_ = true;
  bool stopping_ = false;
  bool critical_ = false;
  std::string process_id_;
  std::vector<std::shared_ptr<StreamInfo>> streams_;
  SymbolTable local_symbols_;
  int sigint_timeout_secs_ = 0;
  int sigterm_timeout_secs_ = 0;
  std::string user_;
  std::string group_;
  bool interactive_ = false;
  Terminal interactive_terminal_;
  toolbelt::FileDescriptor interactive_this_end_;
  toolbelt::FileDescriptor interactive_proc_end_;
};

class StaticProcess : public Process {
public:
  StaticProcess(co::CoroutineScheduler &scheduler,
                std::shared_ptr<ClientHandler> client,
                const stagezero::control::LaunchStaticProcessRequest &&req);

  absl::Status Start(co::Coroutine *c) override;
  bool WillNotify() const override { return req_.opts().notify(); }
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
  Zygote(co::CoroutineScheduler &scheduler,
         std::shared_ptr<ClientHandler> client,
         const stagezero::control::LaunchStaticProcessRequest &&req)
      : StaticProcess(scheduler, client, std::move(req)) {}

  absl::Status Start(co::Coroutine *c) override;

  // Note that this is a completely blocking function that doesn't use
  // a coroutine.  It is called from other coroutines but since it talks
  // to a process through a socket, we can't interleave the messages
  // if multiple spawns happen at the same time.
  absl::StatusOr<int>
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

  void ForeachVirtualProcess(std::function<void(std::shared_ptr<VirtualProcess>)> fn) {
    for (auto& v : virtual_processes_) {
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
  VirtualProcess(co::CoroutineScheduler &scheduler,
                 std::shared_ptr<ClientHandler> client,
                 const stagezero::control::LaunchVirtualProcessRequest &&req);

  absl::Status Start(co::Coroutine *c) override;
  bool WillNotify() const override { return req_.opts().notify(); }
  int StartupTimeoutSecs() const override {
    return req_.opts().startup_timeout_secs();
  }

  void CloseNotifyPipe() {
    notify_pipe_.WriteFd().Reset();
  }

  void Notify(int64_t status) override {
    (void)write(notify_pipe_.WriteFd().Fd(), &status, 8);
  }

  bool IsVirtual() const override { return true; }

private:
  int Wait() override;
  int WaitForZygoteNotification(co::Coroutine* c);

  stagezero::control::LaunchVirtualProcessRequest req_;
  toolbelt::Pipe notify_pipe_;
  std::shared_ptr<Zygote> zygote_;
};

} // namespace adastra::stagezero
