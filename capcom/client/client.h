// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.
#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/alarm.h"
#include "common/states.h"
#include "common/event.h"
#include "common/stream.h"
#include "common/vars.h"
#include "common/tcp_client.h"
#include "coroutine.h"
#include "proto/capcom.pb.h"
#include "proto/config.pb.h"
#include "toolbelt/sockets.h"
#include <variant>

namespace stagezero::capcom::client {

enum class ClientMode {
  kBlocking,
  kNonBlocking,
};

constexpr int32_t kDefaultStartupTimeout = 2;
constexpr int32_t kDefaultSigIntShutdownTimeout = 2;
constexpr int32_t kDefaultSigTermShutdownTimeout = 4;

struct StaticProcess {
  std::string name;
  std::string description;
  std::string executable;
  std::string compute; // Where to run.  Empty is localhost.
  std::vector<Variable> vars;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  bool notify = false;
  std::vector<Stream> streams;
};

struct Zygote {
  std::string name;
  std::string description;
  std::string executable;
  std::string compute; // Where to run.  Empty is localhost.
  std::vector<Variable> vars;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  std::vector<Stream> streams;
};

struct VirtualProcess {
  std::string name;
  std::string description;
  std::string zygote;
  std::string dso;
  std::string main_func;
  std::string compute; // Where to run.  Empty is localhost.
  std::vector<Variable> vars;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  std::vector<Stream> streams;
};

struct SubsystemOptions {
  std::vector<StaticProcess> static_processes;
  std::vector<Zygote> zygotes;
  std::vector<VirtualProcess> virtual_processes;

  std::vector<Variable> vars;
  std::vector<std::string> args;
  std::vector<std::string> children;
};

class Client : public TCPClient<capcom::proto::Request, capcom::proto::Response, stagezero::proto::Event> {
public:
  Client(ClientMode mode = ClientMode::kBlocking, co::Coroutine *co = nullptr) : TCPClient<capcom::proto::Request, capcom::proto::Response, stagezero::proto::Event>(co), mode_(mode) {}
  ~Client() = default;

  absl::Status Init(toolbelt::InetAddress addr, const std::string &name,
                    co::Coroutine *c = nullptr);

  absl::Status AddCompute(const std::string &name,
                          const toolbelt::InetAddress &addr,
                          co::Coroutine *c = nullptr);

  absl::Status RemoveCompute(const std::string &name,
                             co::Coroutine *c = nullptr);

  absl::Status AddSubsystem(const std::string &name,
                            const SubsystemOptions &options,
                            co::Coroutine *c = nullptr);

  absl::Status RemoveSubsystem(const std::string &name, bool recursive,
                               co::Coroutine *c = nullptr);

  absl::Status StartSubsystem(const std::string &name,
                              co::Coroutine *c = nullptr);
  absl::Status StopSubsystem(const std::string &name,
                             co::Coroutine *c = nullptr);


  absl::Status AddGlobalVariable(const Variable& var,
                          co::Coroutine *c = nullptr);

  // Wait for an incoming event.
  absl::StatusOr<std::shared_ptr<Event>> WaitForEvent(co::Coroutine *c = nullptr) {
    return ReadEvent(c);
  }
  absl::StatusOr<std::shared_ptr<Event>> ReadEvent(co::Coroutine *c = nullptr);

  absl::Status Abort(const std::string &reason, co::Coroutine *c = nullptr);

private:
  absl::Status WaitForSubsystemState(const std::string &subsystem,
                                     AdminState admin_state,
                                     OperState oper_state);
  ClientMode mode_;
};

} // namespace stagezero::capcom::client
