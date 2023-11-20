// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "coroutine.h"
#include "proto/config.pb.h"
#include "proto/control.pb.h"
#include "stagezero/symbols.h"
#include "toolbelt/fd.h"
#include "toolbelt/sockets.h"

#include <memory>
#include <string>

namespace stagezero {

class ClientHandler;

struct StreamInfo {
  control::StreamControl::Direction direction;
  control::StreamControl::Disposition disposition;
  toolbelt::FileDescriptor read_fd;  // Read end of pipe or tty.
  toolbelt::FileDescriptor write_fd; // Write end.
  int fd;                            // Process fd to map to.
};

// Ownership of processes is complex due to the fact that the
// process will run coroutines.  We store shared_ptrs to the
// Processes in the StageZero object and also capture the
// shared_ptrs in the coroutines.  Thus the Process instance
// will be deleted when the Process has been removed from the
// StageZero map and all coroutines have finished running.
class Process : public std::enable_shared_from_this<Process> {
public:
  Process(co::CoroutineScheduler &scheduler, std::shared_ptr<ClientHandler> client, std::string name);
  virtual ~Process() { std::cerr << "Process " << name_ << " destructed" << std::endl; }

  virtual absl::Status Start(co::Coroutine* c) = 0;
  virtual absl::Status Stop(co::Coroutine* c);
  void KillNow();

  virtual bool IsZygote() const { return false; }
  absl::Status SendInput(int fd, const std::string &data, co::Coroutine *c);

  absl::Status CloseFileDescriptor(int fd);

  const std::string &GetId() const { return process_id_; }
  
  const std::string& Name() const { return name_; }

  int WaitLoop(co::Coroutine *c, int timeout_secs);

  bool IsStopping() const { return stopping_; }
  bool IsRunning() const { return running_; }
  int GetPid() const { return pid_; }
  void SetPid(int pid) { pid_ = pid; }
  void SetProcessId();
  virtual bool WillNotify() const = 0;
  virtual int StartupTimeoutSecs() const = 0;

  const std::shared_ptr<StreamInfo> FindNotifyStream() const;

  const std::vector<std::shared_ptr<StreamInfo>>& GetStreams() const { return streams_; }
  
  void SetSignalTimeouts(int sigint, int sigterm) {
    sigint_timeout_secs_ = sigint;
    sigterm_timeout_secs_ = sigterm;
  }

  int SigIntTimeoutSecs() const { return sigint_timeout_secs_; }
  int SigTermTimeoutSecs() const { return sigterm_timeout_secs_; }

protected:
  virtual int Wait() = 0;
  absl::Status BuildStreams(
      const google::protobuf::RepeatedPtrField<control::StreamControl> &streams,
      bool notify);

  co::CoroutineScheduler &scheduler_;
  std::shared_ptr<ClientHandler> client_;
  std::string name_;
  int pid_ = 0;
  bool running_ = true;
  bool stopping_ = false;
  std::string process_id_;
  std::vector<std::shared_ptr<StreamInfo>> streams_;
  SymbolTable local_symbols_;
  int sigint_timeout_secs_ = 0;
  int sigterm_timeout_secs_ = 0;
};

class StaticProcess : public Process {
public:
  StaticProcess(co::CoroutineScheduler &scheduler, std::shared_ptr<ClientHandler> client,
                const stagezero::control::LaunchStaticProcessRequest &&req);

  absl::Status Start(co::Coroutine* c) override;
  bool WillNotify() const override { return req_.opts().notify(); }
  int StartupTimeoutSecs() const override { return req_.opts().startup_timeout_secs(); }

protected:
  absl::Status StartInternal(const std::vector<std::string> extra_env_vars,
                             bool send_start_event);
  absl::Status ForkAndExec(const std::vector<std::string> extra_env_vars);
  int Wait() override;
  stagezero::control::LaunchStaticProcessRequest req_;
};

class Zygote : public StaticProcess {
public:
  Zygote(co::CoroutineScheduler &scheduler, std::shared_ptr<ClientHandler> client,
         const stagezero::control::LaunchStaticProcessRequest &&req)
      : StaticProcess(scheduler, client, std::move(req)) {
    std::cerr << "Zygote created " << req_.DebugString() << std::endl;
  }

  absl::Status Start(co::Coroutine* c) override;

  absl::StatusOr<int>
  Spawn(const stagezero::control::LaunchVirtualProcessRequest &req,
  const std::vector<std::shared_ptr<StreamInfo>>& streams,

        co::Coroutine *c);

  bool IsZygote() const override { return true; }

  void SetControlSocket(toolbelt::UnixSocket s) {
    control_socket_ = std::move(s);
  }

private:
  std::pair<std::string, int> BuildControlSocketName();

  toolbelt::UnixSocket control_socket_;
};

class VirtualProcess : public Process {
public:
  VirtualProcess(co::CoroutineScheduler &scheduler, std::shared_ptr<ClientHandler> client,
                 const stagezero::control::LaunchVirtualProcessRequest &&req);

  absl::Status Start(co::Coroutine* c) override;
  bool WillNotify() const override { return req_.opts().notify(); }
  int StartupTimeoutSecs() const override { return req_.opts().startup_timeout_secs(); }

private:
  int Wait() override;

  stagezero::control::LaunchVirtualProcessRequest req_;
};

} // namespace stagezero
