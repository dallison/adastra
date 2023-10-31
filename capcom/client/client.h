#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "coroutine.h"
#include "proto/capcom.pb.h"
#include "proto/config.pb.h"
#include "toolbelt/sockets.h"

namespace stagezero::capcom::client {

struct Variable {
  std::string name;
  std::string value;
  bool exported = false;
};

constexpr int32_t kDefaultStartupTimeout = 2;
constexpr int32_t kDefaultSigIntShutdownTimeout = 2;
constexpr int32_t kDefaultSigTermShutdownTimeout = 4;

struct StaticProcess {
  std::string name;
  std::string description;
  std::string executable;
  std::vector<Variable> vars;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  bool notify = false;
};

struct Zygote {
  std::string name;
  std::string executable;
  std::vector<Variable> vars;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  bool notify = false;
  };

struct VirtualProcess {
  std::string name;
  std::string dso;
  std::string main_func;
  std::vector<Variable> vars;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  bool notify = false;
};

struct SubsystemOptions {
  std::vector<StaticProcess> static_processes;
  std::vector<Zygote> zygotes;
  std::vector<VirtualProcess> virtual_processes;

  std::vector<Variable> vars;
  std::vector<std::string> args;
  std::vector<std::string> children;
};

class Client {
public:
  Client(co::Coroutine *co = nullptr) : co_(co) {}
  ~Client() = default;

  absl::Status Init(toolbelt::InetAddress addr, const std::string &name);

  absl::Status AddSubsystem(const std::string &name,
                            const SubsystemOptions &options);

  absl::Status RemoveSubsystem(const std::string& name, bool recursive);

    absl::Status StartSubsystem(const std::string& name);
    absl::Status StopSubsystem(const std::string& name);

private:
  static constexpr size_t kMaxMessageSize = 4096;

  absl::Status
  SendRequestReceiveResponse(const stagezero::capcom::proto::Request &req,
                             stagezero::capcom::proto::Response &response);

  std::string name_ = "";
  co::Coroutine *co_;
  toolbelt::TCPSocket command_socket_;
  char command_buffer_[kMaxMessageSize];
};

} // namespace stagezero::capcom::client
