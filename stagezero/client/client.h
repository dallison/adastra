#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "coroutine.h"
#include "stagezero/proto/config.pb.h"
#include "stagezero/proto/control.pb.h"
#include "toolbelt/sockets.h"

#include <variant>

namespace stagezero {

struct Variable {
  std::string name;
  std::string value;
  bool exported = false;
};

struct Stream {
  enum class Disposition {
    kClient,
    kFile,
    kFd,
    kClose,
  };
  enum class Direction {
    kInput,
    kOutput,
  };

  int stream_fd;
  bool tty = false;
  Disposition disposition = Disposition::kClient;
  Direction direction = Direction::kOutput;
  std::variant<std::string, int> data;
};

constexpr int32_t kDefaultStartupTimeout = 2;
constexpr int32_t kDefaultSigIntShutdownTimeout = 2;
constexpr int32_t kDefaultSigTermShutdownTimeout = 4;

struct LaunchOptions {
  std::string description;
  std::vector<Variable> vars;
  std::vector<std::string> args;
  std::vector<Stream> streams;
  int32_t startup_timeout_secs = kDefaultStartupTimeout;
  int32_t sigint_shutdown_timeout_secs = kDefaultSigIntShutdownTimeout;
  int32_t sigterm_shutdown_timeout_secs = kDefaultSigTermShutdownTimeout;
  bool notify = false;
};

class Client {
public:
  Client(co::Coroutine *co = nullptr) : co_(co) {}
  ~Client() = default;

  absl::Status Init(toolbelt::InetAddress addr, const std::string &name);

  absl::StatusOr<std::pair<std::string, int>> LaunchStaticProcess(const std::string &name,
                                                  const std::string &executable,
                                                  LaunchOptions opts = {}) {
    return LaunchStaticProcessInternal(name, executable, std::move(opts),
                                       false);
  }

  absl::StatusOr<std::pair<std::string, int>> LaunchZygote(const std::string &name,
                                           const std::string &executable,
                                           LaunchOptions opts = {}) {
    // Zygotes always notify.
    opts.notify = true;
    return LaunchStaticProcessInternal(name, executable, std::move(opts), true);
  }

  // Launch a virtual process loaded from a shared library.
  absl::StatusOr<std::pair<std::string, int>> LaunchVirtualProcess(const std::string &name,
                                                   const std::string &zygote,
                                                   const std::string &dso,
                                                   const std::string &main_func,
                                                   LaunchOptions opts = {});

  // Launch a virtual process that is linked with the zygote.
  absl::StatusOr<std::pair<std::string, int>> LaunchVirtualProcess(const std::string &name,
                                                   const std::string &zygote,
                                                   const std::string &main_func,
                                                   LaunchOptions opts = {}) {
    return LaunchVirtualProcess(name, zygote, "", main_func, opts);
  }

  absl::Status StopProcess(const std::string &process_id);

  // Wait for an incoming event.
  absl::StatusOr<stagezero::control::Event> WaitForEvent();

  absl::Status SendInput(const std::string &process_id, int fd,
                         const std::string &data);

  absl::Status CloseProcessFileDescriptor(const std::string &process_id,
                                          int fd);

  absl::Status SetGlobalVariable(std::string name, std::string value,
                                 bool exported);
  absl::StatusOr<std::pair<std::string, bool>>
  GetGlobalVariable(std::string name);

private:
  static constexpr size_t kMaxMessageSize = 4096;

  absl::StatusOr<std::pair<std::string, int>>
  LaunchStaticProcessInternal(const std::string &name,
                              const std::string &executable, LaunchOptions opts,
                              bool zygote);
  absl::Status
  SendRequestReceiveResponse(const stagezero::control::Request &req,
                             stagezero::control::Response &response);

  void BuildLaunchOptions(const std::string &name,
                          stagezero::config::LaunchOptions *options,
                          LaunchOptions opts) const;
  void BuildStream(stagezero::control::StreamControl *out,
                   const Stream &in) const;

  std::string name_ = "";
  co::Coroutine *co_;
  toolbelt::TCPSocket command_socket_;
  char command_buffer_[kMaxMessageSize];

  toolbelt::TCPSocket event_socket_;
  char event_buffer_[kMaxMessageSize];
};
} // namespace stagezero
