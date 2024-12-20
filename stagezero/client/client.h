// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/cgroup.h"
#include "common/event.h"
#include "common/namespace.h"
#include "common/parameters.h"
#include "common/stream.h"
#include "common/tcp_client.h"
#include "common/vars.h"
#include "coroutine.h"
#include "proto/config.pb.h"
#include "proto/control.pb.h"
#include "stagezero/telemetry/telemetry.h"
#include "toolbelt/sockets.h"

#include <variant>

namespace adastra::stagezero {

constexpr int32_t kDefaultStartupTimeout = 2;
constexpr int32_t kDefaultTelemetryShutdownTimeout = 2;
constexpr int32_t kDefaultSigIntShutdownTimeout = 2;
constexpr int32_t kDefaultSigTermShutdownTimeout = 4;

struct ProcessOptions {
  std::string description;
  std::vector<adastra::Variable> vars;
  std::vector<std::string> args;
  std::vector<adastra::Stream> streams;
  std::vector<parameters::Parameter> parameters;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t telemetry_shutdown_timeout_secs = kDefaultTelemetryShutdownTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  bool notify = false;
  bool telemetry = false;
  bool interactive = false;
  adastra::Terminal interactive_terminal;
  std::string user;
  std::string group;
  bool critical;
  std::string cgroup;
  bool detached;
  std::optional<Namespace> ns;
};

class Client : public adastra::TCPClient<control::Request, control::Response,
                                         control::Event> {
public:
  Client(co::Coroutine *co = nullptr)
      : TCPClient<control::Request, control::Response, control::Event>(co) {}
  ~Client() = default;

  absl::Status Init(toolbelt::InetAddress addr, const std::string &name,
                    int event_mask = adastra::kAllEvents,
                    const std::string &compute = "localhost",
                    co::Coroutine *co = nullptr);

  // Wait for an incoming event.
  absl::StatusOr<std::shared_ptr<control::Event>>
  WaitForEvent(co::Coroutine *c = nullptr) {
    return ReadEvent(c);
  }

  absl::StatusOr<std::shared_ptr<control::Event>>
  ReadEvent(co::Coroutine *c = nullptr) {
    absl::StatusOr<std::shared_ptr<control::Event>> event = ReadProtoEvent(c);
    if (!event.ok()) {
      if (event.status().code() == absl::StatusCode::kCancelled) {
        // On error or EOF, return a disconnect event.
        auto event = std::make_shared<control::Event>();
        event->mutable_disconnect()->set_compute(compute_);
        Close();
        return event;
      }
      return event.status();
    }
    return *event;
  }

  // Returns a pair containing a unique string process-id and the PID of
  // the running process.
  absl::StatusOr<std::pair<std::string, int>>
  LaunchStaticProcess(const std::string &name, const std::string &executable,
                      ProcessOptions opts = {}, co::Coroutine *co = nullptr) {
    return LaunchStaticProcessInternal(name, executable, std::move(opts), false,
                                       co);
  }

  absl::StatusOr<std::pair<std::string, int>>
  LaunchZygote(const std::string &name, const std::string &executable,
               ProcessOptions opts = {}, co::Coroutine *co = nullptr) {
    // Zygotes always notify.
    opts.notify = true;

    // Zygotes use telemetry.
    opts.telemetry = true;
    return LaunchStaticProcessInternal(name, executable, std::move(opts), true,
                                       co);
  }

  // Launch a virtual process loaded from a shared library.
  absl::StatusOr<std::pair<std::string, int>>
  LaunchVirtualProcess(const std::string &name, const std::string &zygote,
                       const std::string &dso, const std::string &main_func,
                       ProcessOptions opts = {}, co::Coroutine *co = nullptr);

  // Launch a virtual process that is linked with the zygote.
  absl::StatusOr<std::pair<std::string, int>>
  LaunchVirtualProcess(const std::string &name, const std::string &zygote,
                       const std::string &main_func, ProcessOptions opts = {},
                       co::Coroutine *co = nullptr) {
    return LaunchVirtualProcess(name, zygote, "", main_func, opts, co);
  }

  absl::Status StopProcess(const std::string &process_id,
                           co::Coroutine *co = nullptr);

  absl::Status SendInput(const std::string &process_id, int fd,
                         const std::string &data, co::Coroutine *co = nullptr);

  absl::Status CloseProcessFileDescriptor(const std::string &process_id, int fd,
                                          co::Coroutine *co = nullptr);

  absl::Status SetGlobalVariable(std::string name, std::string value,
                                 bool exported, co::Coroutine *co = nullptr);
  absl::StatusOr<std::pair<std::string, bool>>
  GetGlobalVariable(std::string name, co::Coroutine *co = nullptr);

  absl::Status Abort(const std::string &reason, bool emergency,
                     co::Coroutine *co = nullptr);

  absl::Status RegisterCgroup(const Cgroup &cgroup,
                              co::Coroutine *co = nullptr);
  absl::Status FreezeCgroup(const std::string &cgroup,
                            co::Coroutine *co = nullptr);
  absl::Status ThawCgroup(const std::string &cgroup,
                          co::Coroutine *co = nullptr);
  absl::Status KillCgroup(const std::string &cgroup,
                          co::Coroutine *co = nullptr);
  absl::Status SetParameter(const std::string &name, const parameters::Value &v,
                            co::Coroutine *c = nullptr);

  absl::Status DeleteParameters(const std::vector<std::string> &name,
                               co::Coroutine *c = nullptr);
  absl::Status
  UploadParameters(const std::vector<parameters::Parameter> &params,
                   co::Coroutine *c = nullptr);

  absl::Status SendTelemetryCommand(const std::string &process_id,
                                     const ::stagezero::TelemetryCommand &cmd,
                                     co::Coroutine *c = nullptr);
 absl::Status SendTelemetryCommand(const std::string &process_id,
                                     const adastra::proto::telemetry::Command &cmd,
                                     co::Coroutine *c = nullptr);
private:
  absl::StatusOr<std::pair<std::string, int>> LaunchStaticProcessInternal(
      const std::string &name, const std::string &executable,
      ProcessOptions opts, bool zygote, co::Coroutine *co);

  void BuildProcessOptions(const std::string &name,
                           adastra::stagezero::config::ProcessOptions *options,
                           ProcessOptions opts) const;
  std::string compute_;
};
} // namespace adastra::stagezero
