// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.
#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/alarm.h"
#include "common/capability.h"
#include "common/cgroup.h"
#include "common/event.h"
#include "common/parameters.h"
#include "common/scheduler.h"
#include "common/states.h"
#include "common/stream.h"
#include "common/subsystem_status.h"
#include "common/tcp_client.h"
#include "common/vars.h"
#include "coroutine.h"
#include "proto/capcom.pb.h"
#include "proto/config.pb.h"
#include "stagezero/telemetry/telemetry.h"
#include "toolbelt/sockets.h"
#include <variant>

namespace adastra::capcom::client {

enum class ClientMode {
  kBlocking,
  kNonBlocking,
};

enum class RestartPolicy {
  kAutomatic,
  kManual,
  kProcessOnly,
};

static constexpr int kDefaultMaxRestarts = 3;

constexpr int32_t kDefaultStartupTimeout = 2;
constexpr int32_t kDefaultTelemetryShutdownTimeout = 2;
constexpr int32_t kDefaultSigIntShutdownTimeout = 2;
constexpr int32_t kDefaultSigTermShutdownTimeout = 4;

struct StaticProcess {
  std::string name;
  std::string description;
  std::string executable;
  std::string compute; // Where to run.  Empty is localhost.
  std::vector<Variable> vars;
  std::vector<parameters::Parameter> parameters;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t telemetry_shutdown_timeout_secs = kDefaultTelemetryShutdownTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  bool notify = false;
  bool telemetry = false;
  std::vector<Stream> streams;
  std::string user;
  std::string group;
  bool interactive = false;
  bool oneshot = false;
  std::string cgroup = "";
  int32_t max_restarts = kDefaultMaxRestarts;
  KernelSchedulerPolicy kernel_scheduler_policy;
  std::vector<int> cpus;
  CapabilitySet capabilities;
};

struct Zygote {
  std::string name;
  std::string description;
  std::string executable;
  std::string compute; // Where to run.  Empty is localhost.
  std::vector<Variable> vars;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t telemetry_shutdown_secs = kDefaultTelemetryShutdownTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  std::vector<Stream> streams;
  std::string user;
  std::string group;
  std::string cgroup = "";
  int32_t max_restarts = kDefaultMaxRestarts;
  KernelSchedulerPolicy kernel_scheduler_policy;
  std::vector<int> cpus;
  CapabilitySet capabilities;
};

struct VirtualProcess {
  std::string name;
  std::string description;
  std::string zygote;
  std::string dso;
  std::string main_func;
  std::string compute; // Where to run.  Empty is localhost.
  std::vector<Variable> vars;
  std::vector<parameters::Parameter> parameters;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t telemetry_shutdown_timeout_secs = kDefaultTelemetryShutdownTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  bool notify = false;
  bool telemetry = false;
  std::vector<Stream> streams;
  std::string user;
  std::string group;
  std::string cgroup = "";
  int32_t max_restarts = kDefaultMaxRestarts;
  KernelSchedulerPolicy kernel_scheduler_policy;
  std::vector<int> cpus;
  CapabilitySet capabilities;
};

struct SubsystemOptions {
  std::vector<StaticProcess> static_processes;
  std::vector<Zygote> zygotes;
  std::vector<VirtualProcess> virtual_processes;

  std::vector<Variable> vars;
  std::vector<Stream> streams;
  std::vector<std::string> args;
  std::vector<std::string> children;

  int max_restarts = kDefaultMaxRestarts;
  bool critical = false;
  RestartPolicy restart_policy = RestartPolicy::kAutomatic;
};

enum class RunMode {
  kNoninteractive,
  kInteractive,
  kProcessOnly, // Restart only the process that exited
};

enum class ComputeConnectionPolicy {
  kDynamic,
  kStatic,
};

class Client : public TCPClient<capcom::proto::Request, capcom::proto::Response,
                                adastra::proto::Event> {
public:
  Client(ClientMode mode = ClientMode::kBlocking, co::Coroutine *co = nullptr)
      : TCPClient<capcom::proto::Request, capcom::proto::Response,
                  adastra::proto::Event>(co),
        mode_(mode) {}
  ~Client() = default;

  absl::Status Init(toolbelt::InetAddress addr, const std::string &name,
                    int event_mask, co::Coroutine *c = nullptr);

  absl::Status AddCompute(const std::string &name,
                          const toolbelt::InetAddress &addr,
                          ComputeConnectionPolicy connection_policy =
                              ComputeConnectionPolicy::kDynamic,
                          const std::vector<Cgroup> &cgroups = {},
                          co::Coroutine *c = nullptr);

  absl::Status RemoveCompute(const std::string &name,
                             co::Coroutine *c = nullptr);

  absl::Status AddSubsystem(const std::string &name,
                            const SubsystemOptions &options,
                            co::Coroutine *c = nullptr);

  absl::Status RemoveSubsystem(const std::string &name, bool recursive,
                               co::Coroutine *c = nullptr);

  absl::Status StartSubsystem(const std::string &name,
                              RunMode mode = RunMode::kNoninteractive,
                              Terminal *terminal = nullptr,
                              co::Coroutine *c = nullptr);
  absl::Status StopSubsystem(const std::string &name,
                             co::Coroutine *c = nullptr);

  absl::Status RestartSubsystem(const std::string &name,
                                co::Coroutine *c = nullptr);

  absl::Status RestartProcesses(const std::string &subsystem,
                                const std::vector<std::string> &processes,
                                co::Coroutine *c = nullptr);

  absl::Status AddGlobalVariable(const Variable &var,
                                 co::Coroutine *c = nullptr);

  absl::Status SetParameter(const std::string &name, const parameters::Value &v,
                            co::Coroutine *c = nullptr);

  absl::Status DeleteParameters(const std::vector<std::string> &names = {},
                                co::Coroutine *c = nullptr);

  absl::StatusOr<std::vector<parameters::Parameter>>
  GetParameters(const std::vector<std::string> &names = {},
                co::Coroutine *c = nullptr);

  absl::StatusOr<parameters::Value> GetParameter(const std::string &name,
                                                 co::Coroutine *c = nullptr) {
    auto paras = GetParameters({name}, c);
    if (!paras.ok()) {
      return paras.status();
    }
    if (paras->empty()) {
      return absl::InternalError("Parameter not found");
    }
    return paras->front().value;
  }

  absl::Status
  UploadParameters(const std::vector<parameters::Parameter> &params,
                   co::Coroutine *c = nullptr);

  absl::StatusOr<std::vector<SubsystemStatus>>
  GetSubsystems(co::Coroutine *c = nullptr);

  absl::StatusOr<std::vector<Alarm>> GetAlarms(co::Coroutine *c = nullptr);

  // Wait for an incoming event.
  absl::StatusOr<std::shared_ptr<Event>>
  WaitForEvent(co::Coroutine *c = nullptr) {
    return ReadEvent(c);
  }
  absl::StatusOr<std::shared_ptr<Event>> ReadEvent(co::Coroutine *c = nullptr);

  absl::Status Abort(const std::string &reason, bool emergency,
                     co::Coroutine *c = nullptr);

  absl::Status SendInput(const std::string &subsystem,
                         const std::string &process, int fd,
                         const std::string &data, co::Coroutine *c = nullptr);

  absl::Status CloseFd(const std::string &subsystem,
                       const std::string &process_name, int fd,
                       co::Coroutine *c = nullptr);

  absl::Status FreezeCgroup(const std::string &compute,
                            const std::string &cgroup,
                            co::Coroutine *co = nullptr);
  absl::Status ThawCgroup(const std::string &compute, const std::string &cgroup,
                          co::Coroutine *co = nullptr);
  absl::Status KillCgroup(const std::string &compute, const std::string &cgroup,
                          co::Coroutine *co = nullptr);

  absl::Status
  SendTelemetryCommandToSubsystem(const std::string &subsystem,
                                  const ::stagezero::TelemetryCommand &command,
                                  co::Coroutine *c = nullptr);
  absl::Status SendTelemetryCommandToProcess(
      const std::string &subsystem, const std::string &process_id,
      const ::stagezero::TelemetryCommand &command, co::Coroutine *c = nullptr);

private:
  absl::Status WaitForSubsystemState(const std::string &subsystem,
                                     AdminState admin_state,
                                     OperState oper_state,
                                     co::Coroutine *c = nullptr);
  ClientMode mode_;
};

} // namespace adastra::capcom::client
