#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/alarm.h"
#include "common/states.h"
#include "common/vars.h"
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
  std::string compute;      // Where to run.  Empty is localhost.
  std::vector<Variable> vars;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  bool notify = false;
};

struct Zygote {
  std::string name;
  std::string description;
  std::string executable;
  std::string compute;      // Where to run.  Empty is localhost.
  std::vector<Variable> vars;
  std::vector<std::string> args;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  bool notify = false;
};

struct VirtualProcess {
  std::string name;
  std::string description;
  std::string zygote;
  std::string dso;
  std::string main_func;
  std::string compute;      // Where to run.  Empty is localhost.
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

enum class EventType {
  kSubsystemStatus,
  kAlarm,
};

struct ProcessStatus {
  std::string name;
  std::string process_id;
  int pid;
  bool running;
};

struct SubsystemStatusEvent {
  std::string subsystem;
  AdminState admin_state;
  OperState oper_state;
  std::vector<ProcessStatus> processes;
};

// Alarm is defined in common/alarm.h

struct Event {
  EventType type;
  std::variant<SubsystemStatusEvent, Alarm> event;
};

class Client {
public:
  Client(ClientMode mode = ClientMode::kBlocking, co::Coroutine *co = nullptr) : mode_(mode), co_(co) {}
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

  toolbelt::FileDescriptor GetEventFd() const {
    return event_socket_.GetFileDescriptor();
  }

  // Wait for an incoming event.
  absl::StatusOr<Event> WaitForEvent(co::Coroutine *c = nullptr) {
    return ReadEvent(c);
  }
  absl::StatusOr<Event> ReadEvent(co::Coroutine *c = nullptr);

  absl::Status Abort(const std::string& reason, co::Coroutine *c = nullptr);
  
private:
  static constexpr size_t kMaxMessageSize = 4096;

absl::Status WaitForSubsystemState(const std::string& subsystem,
                                           AdminState admin_state,
                                           OperState oper_state);
  absl::Status
  SendRequestReceiveResponse(const stagezero::capcom::proto::Request &req,
                             stagezero::capcom::proto::Response &response,
                             co::Coroutine *c);

  std::string name_ = "";
  ClientMode mode_;
  co::Coroutine *co_;
  toolbelt::TCPSocket command_socket_;
  char command_buffer_[kMaxMessageSize];

  toolbelt::TCPSocket event_socket_;
  char event_buffer_[kMaxMessageSize];
};

} // namespace stagezero::capcom::client
