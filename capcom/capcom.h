// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "capcom/bitset.h"
#include "capcom/client_handler.h"
#include "capcom/subsystem.h"
#include "common/cgroup.h"
#include "common/event.h"
#include "common/parameters.h"
#include "common/vars.h"
#include "proto/log.pb.h"
#include "stagezero/client/client.h"
#include "toolbelt/logging.h"
#include "toolbelt/pipe.h"
#include "toolbelt/sockets.h"

#include <map>
#include <memory>
#include <string>

namespace adastra::capcom {

// Values sent to notification pipe.
constexpr int64_t kReady = 1;
constexpr int64_t kStopped = 2;

class ClientHandler;

struct Compute {
  std::string name;
  toolbelt::InetAddress addr;
  std::vector<Cgroup> cgroups;
};

class Capcom {
public:
  Capcom(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
         bool log_to_output, int local_stagezero_port,
         std::string log_file_name = "", std::string log_level = "",
         bool test_mode = false, int notify_fd = -1);
  ~Capcom();

  absl::Status Run();
  void Stop();

  void EnterTestMode() { test_mode_ = true; }

private:
  friend class ClientHandler;
  friend class Subsystem;
  friend class Process;

  absl::Status HandleIncomingConnection(toolbelt::TCPSocket &listen_socket,
                                        co::Coroutine *c);

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) {
    coroutines_.insert(std::move(c));
  }

  void Log(const adastra::proto::LogMessage &msg);

  void CloseHandler(std::shared_ptr<ClientHandler> handler);
  void ListenerCoroutine(toolbelt::TCPSocket &listen_socket, co::Coroutine *c);
  void LoggerCoroutine(co::Coroutine *c);
  void LoggerFlushCoroutine(co::Coroutine *c);

  bool AddCompute(std::string name, const Compute &compute) {
    auto[it, inserted] =
        computes_.emplace(std::make_pair(std::move(name), compute));
    return inserted;
  }

  absl::Status RemoveCompute(const std::string &name) {
    auto it = computes_.find(name);
    if (it == computes_.end()) {
      return absl::InternalError(absl::StrFormat("No such compute %s", name));
    }
    computes_.erase(it);
    return absl::OkStatus();
  }

  const Compute *FindCompute(const std::string &name) const {
    if (name.empty()) {
      return &local_compute_;
    }
    auto it = computes_.find(name);
    if (it == computes_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  bool AddSubsystem(std::string name, std::shared_ptr<Subsystem> subsystem) {
    auto[it, inserted] = subsystems_.emplace(
        std::make_pair(std::move(name), std::move(subsystem)));
    return inserted;
  }

  absl::Status RemoveSubsystem(const std::string &name) {
    auto it = subsystems_.find(name);
    if (it == subsystems_.end()) {
      return absl::InternalError(absl::StrFormat("No such subsystem %s", name));
    }
    subsystems_.erase(it);
    return absl::OkStatus();
  }

  std::shared_ptr<Subsystem> FindSubsystem(const std::string &name) const {
    auto it = subsystems_.find(name);
    if (it == subsystems_.end()) {
      return nullptr;
    }
    return it->second;
  }

  bool AddZygote(std::string name, Zygote *zygote) {
    auto[it, inserted] =
        zygotes_.emplace(std::make_pair(std::move(name), zygote));
    return inserted;
  }

  Zygote *FindZygote(const std::string &name) {
    auto it = zygotes_.find(name);
    if (it == zygotes_.end()) {
      return nullptr;
    }
    return it->second;
  }

  absl::Status RemoveZygote(const std::string &name) {
    auto it = zygotes_.find(name);
    if (it == zygotes_.end()) {
      return absl::InternalError(absl::StrFormat("No such zygote %s", name));
    }
    zygotes_.erase(it);
    return absl::OkStatus();
  }

  void SendSubsystemStatusEvent(Subsystem *subsystem);
  void SendParameterUpdateEvent(const std::string &name,
                                const parameters::Value &value);
  void SendParameterDeleteEvent(const std::string &name);

  void SendAlarm(const Alarm &alarm);

  std::vector<Subsystem *> GetSubsystems() const;
  std::vector<Alarm> GetAlarms() const;

  absl::Status Abort(const std::string &reason, bool emergency,
                     co::Coroutine *c);
  absl::Status AddGlobalVariable(const Variable &var, co::Coroutine *c);

  absl::Status RegisterComputeCgroups(stagezero::Client &client,
                                      const Compute &compute, co::Coroutine *c);

  absl::Status PropagateParameterUpdate(const std::string &name,
                                        parameters::Value &value,
                                        co::Coroutine *c);
  absl::Status PropagateParameterDelete(const std::string &name,
                                        co::Coroutine *c);

  void Log(const std::string &source, toolbelt::LogLevel level, const char *fmt,
           ...);

  bool IsEmergencyAborting() { return emergency_aborting_; }
  bool TestMode() const { return test_mode_; }

  absl::Status FreezeCgroup(const std::string &compute,
                            const std::string &cgroup, co::Coroutine *c);
  absl::Status ThawCgroup(const std::string &compute, const std::string &cgroup,
                          co::Coroutine *c);
  absl::Status KillCgroup(const std::string &compute, const std::string &cgroup,
                          co::Coroutine *c);

  absl::Status SetParameter(const std::string &name,
                            const parameters::Value &value);
  absl::Status DeleteParameter(const std::string &name);
  absl::Status
  SetAllParameters(const std::vector<parameters::Parameter> &params);

  absl::Status
  HandleParameterEvent(const adastra::proto::parameters::ParameterEvent &event,
                       co::Coroutine *c);

private:
  co::CoroutineScheduler &co_scheduler_;
  toolbelt::InetAddress addr_;
  bool log_to_output_;
  bool test_mode_;
  toolbelt::FileDescriptor notify_fd_;

  Compute local_compute_;
  absl::flat_hash_map<std::string, Compute> computes_;

  // All coroutines are owned by this set.
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> coroutines_;

  std::vector<std::shared_ptr<ClientHandler>> client_handlers_;
  bool running_ = false;
  absl::flat_hash_map<std::string, std::shared_ptr<Subsystem>> subsystems_;
  absl::flat_hash_map<std::string, Zygote *> zygotes_;
  toolbelt::Logger logger_;

  std::unique_ptr<stagezero::Client> main_client_;

  BitSet client_ids_;

  // Pipe for the logger messages.  Carries serialized log message messages.
  toolbelt::Pipe log_pipe_;
  std::map<uint64_t, std::shared_ptr<adastra::proto::LogMessage>> log_buffer_;
  toolbelt::FileDescriptor log_file_;

  bool emergency_aborting_ = false;

  // Parameters for all running programs.
  parameters::ParameterServer parameters_;
};
} // namespace adastra::capcom
