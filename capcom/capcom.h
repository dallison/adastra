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
#include "stagezero/symbols.h"
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

class Capcom {
public:
  Capcom(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
         bool log_to_output, int local_stagezero_port,
         std::string log_file_name = "", std::string log_level = "",
         bool test_mode = false, int notify_fd = -1,
         bool log_process_output = true);
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
  void FlushLogs();
  void ConnectStaticUmbilical(std::shared_ptr<Compute> compute,
                              co::Coroutine *c);

  std::pair<std::shared_ptr<Compute>, bool> AddCompute(std::string name,
                                                       const Compute &compute) {
    auto [it, inserted] = computes_.emplace(
        std::make_pair(std::move(name), std::make_shared<Compute>(compute)));
    return {it->second, inserted};
  }

  absl::Status RemoveCompute(const std::string &name) {
    auto it = computes_.find(name);
    if (it == computes_.end()) {
      return absl::InternalError(absl::StrFormat("No such compute %s", name));
    }
    computes_.erase(it);
    return absl::OkStatus();
  }

  std::vector<std::string> ListComputes() const {
    std::vector<std::string> names;
    std::transform(computes_.cbegin(), computes_.cend(),
                   std::back_inserter(names),
                   [](const auto &pair) { return pair.first; });
    return names;
  }

  std::shared_ptr<Compute> FindCompute(const std::string &name) const {
    if (name.empty()) {
      return local_compute_;
    }
    auto it = computes_.find(name);
    if (it == computes_.end()) {
      return nullptr;
    }
    return it->second;
  }

  absl::Status AddCgroup(const std::string &compute_name, const Cgroup &cgroup,
                         co::Coroutine *c);
  absl::Status RemoveCgroup(const std::string &compute_name,
                            const std::string &cgroup_name, co::Coroutine *c);
  absl::Status RemoveAllCgroups(const std::string &compute_name);

  std::vector<CgroupAssignment>
  ListCgroupAssignments(const std::string &compute_name,
                        const std::string &cgroup_name) const;

  void AddUmbilical(std::shared_ptr<Compute> compute, bool is_static) {
    auto it = stagezero_umbilicals_.find(compute->name);
    if (it != stagezero_umbilicals_.end()) {
      it->second.IncStaticRefs(+1);
      return;
    }
    stagezero_umbilicals_.emplace(
        compute->name,
        Umbilical{"capcom", logger_, compute,
                  std::make_shared<stagezero::Client>(), is_static});
  }

  void RemoveUmbilical(const std::string &compute, bool dynamic_only) {
    auto it = stagezero_umbilicals_.find(compute);
    if (it == stagezero_umbilicals_.end()) {
      return;
    }
    it->second.IncStaticRefs(-1);
    if (dynamic_only && it->second.IsStatic()) {
      return;
    }
    if (it->second.HasStaticRefs()) {
      return;
    }

    stagezero_umbilicals_.erase(it);
  }

  absl::Status ConnectUmbilical(const std::string &compute, co::Coroutine *c);
  void DisconnectUmbilical(const std::string &compute, bool dynamic_only);

  Umbilical *FindUmbilical(const std::string &compute) {
    auto it = stagezero_umbilicals_.find(compute);
    if (it == stagezero_umbilicals_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  bool AddSubsystem(std::string name, std::shared_ptr<Subsystem> subsystem) {
    auto [it, inserted] = subsystems_.emplace(
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
    auto [it, inserted] =
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
  void SendParameterDeleteEvent(const std::vector<std::string> &names);
  void
  SendTelemetryEvent(const std::string &subsystem,
                     const adastra::stagezero::control::TelemetryEvent &event);

  void SendOutputEvent(int fd, const std::string &name,
                       const std::string &process_id, const std::string &data);
  void SendAlarm(const Alarm &alarm);

  std::vector<Subsystem *> GetSubsystems() const;
  std::vector<Alarm> GetAlarms() const;

  absl::Status Abort(const std::string &reason, bool emergency,
                     co::Coroutine *c);
  absl::Status AddGlobalVariable(const Variable &var, co::Coroutine *c);

  absl::Status RegisterComputeCgroups(std::shared_ptr<stagezero::Client> client,
                                      std::shared_ptr<Compute> compute,
                                      co::Coroutine *c);

  absl::Status RemoveComputeCgroups(std::shared_ptr<stagezero::Client> client,
                                    std::shared_ptr<Compute> compute,
                                    co::Coroutine *c);
  absl::Status PropagateParameterUpdate(const std::string &name,
                                        const parameters::Value &value,
                                        co::Coroutine *c);
  absl::Status PropagateParameterDelete(const std::vector<std::string> &names,
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
  absl::Status DeleteParameters(const std::vector<std::string> &names,
                                co::Coroutine *c);
  absl::Status
  SetAllParameters(const std::vector<parameters::Parameter> &params);
  absl::StatusOr<std::vector<parameters::Parameter>>
  GetParameters(const std::vector<std::string> &names);

  absl::Status
  HandleParameterEvent(const adastra::proto::parameters::ParameterEvent &event,
                       co::Coroutine *c);
  absl::Status
  SendTelemetryCommand(const proto::SendTelemetryCommandRequest &req,
                       co::Coroutine *c);

private:
  co::CoroutineScheduler &co_scheduler_;
  toolbelt::InetAddress addr_;
  bool log_to_output_;
  bool test_mode_;
  toolbelt::FileDescriptor notify_fd_;

  std::shared_ptr<Compute> local_compute_;
  absl::flat_hash_map<std::string, std::shared_ptr<Compute>> computes_;

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
  stagezero::SymbolTable global_symbols_;

  // Connections to the StageZero on each compute.  This is used as the
  // umbilical for common StageZero data like parameters, cgroups, etc.
  absl::flat_hash_map<std::string, Umbilical> stagezero_umbilicals_;

  bool log_process_output_ = true;
};
} // namespace adastra::capcom
