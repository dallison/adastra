#include "stagezero/telemetry/telemetry.h"

namespace stagezero {

void Telemetry::Init(
    co::CoroutineScheduler &scheduler,
    absl::flat_hash_set<std::unique_ptr<co::Coroutine>> *coroutines) {
  // Look for the STAGEZERO_TELEMETRY_FDS environment variable.
  const char *env = getenv("STAGEZERO_TELEMETRY_FDS");
  if (env == nullptr) {
    // No parameters stream.
    return;
  }
  int rfd, wfd;
  sscanf(env, "%d:%d", &rfd, &wfd);
  read_fd_.SetFd(rfd);
  write_fd_.SetFd(wfd);

  // Set up the shutdown trigger.
  if (absl::Status status = shutdown_fd_.Open(); !status.ok()) {
    std::cerr << "Failed to open shutdown trigger: " << status.message()
              << std::endl;
  }
  scheduler_ = &scheduler;
  if (coroutines == nullptr) {
    coroutines_ = &local_coroutines_;
  } else {
    coroutines_ = coroutines;
  }

  local_scheduler_.SetCompletionCallback(
      [this](co::Coroutine *c) { local_coroutines_.erase(c); });
}

bool Telemetry::Wait(co::Coroutine *c) {
  int fd = c->Wait({read_fd_.Fd(), shutdown_fd_.GetPollFd().Fd()}, POLLIN);
  if (fd == shutdown_fd_.GetPollFd().Fd()) {
    return false;
  }
  return true;
}

absl::Status Telemetry::HandleCommand(co::Coroutine *c) {
  // Read length.
  uint32_t len;
  if (!Wait(c)) {
    running_ = false;
    return absl::OkStatus();
  }

  ssize_t n = ::read(read_fd_.Fd(), &len, sizeof(len));
  if (n <= 0) {
    running_ = false;
    return absl::InternalError("Failed to read from telemetry pipe: " +
                               std::to_string(n) + " " + strerror(errno));
  }
  std::vector<char> buffer(len);
  char *buf = buffer.data();
  size_t remaining = len;
  while (remaining > 0) {
    if (!Wait(c)) {
      running_ = false;
      return absl::OkStatus();
    }
    ssize_t n = ::read(read_fd_.Fd(), buf, remaining);
    if (n <= 0) {
      return absl::InternalError("Failed to read from telemetry pipe");
    }
    remaining -= n;
    buf += n;
  }

  adastra::proto::telemetry::Command command;
  if (!command.ParseFromArray(buffer.data(), buffer.size())) {
    return absl::InternalError("Failed to parse command message");
  }

  // Pass this to all modules asking them to parse it.
  for (auto & [ name, module ] : modules_) {
    absl::StatusOr<std::unique_ptr<TelemetryCommand>> cmd =
        module->ParseCommand(command);
    if (!cmd.ok()) {
      return cmd.status();
    }
    if (*cmd != nullptr) {
      // Command is known to module, ask it to handle it.
      return module->HandleCommand(std::move(*cmd), c);
    }
  }
  return absl::InternalError("Unknown telemetry command");
}

void Telemetry::CallEvery(std::chrono::nanoseconds period,
                          std::function<void(co::Coroutine *)> callback) {
  AddCoroutine(std::make_unique<co::Coroutine>(*scheduler_, [
    this, period, callback = std::move(callback)
  ](co::Coroutine * c) {
    // Two coroutines can't wait for the same fd, so we duplicate the shutdown
    // fd.
    toolbelt::FileDescriptor shutdown_fd(dup(shutdown_fd_.GetPollFd().Fd()));
    while (running_) {
      int fd = c->Wait(shutdown_fd.Fd(), POLLIN, period.count());
      if (fd == shutdown_fd.Fd()) {
        running_ = false;
        break;
      }
      callback(c);
    }
  }));
}

void Telemetry::CallAfter(std::chrono::nanoseconds timeout,
                          std::function<void(co::Coroutine *)> callback) {
  AddCoroutine(std::make_unique<co::Coroutine>(*scheduler_, [
    this, timeout, callback = std::move(callback)
  ](co::Coroutine * c) {
    // Two coroutines can't wait for the same fd, so we duplicate the shutdown
    // fd.
    toolbelt::FileDescriptor shutdown_fd(dup(shutdown_fd_.GetPollFd().Fd()));
    int fd = c->Wait(shutdown_fd.Fd(), POLLIN, timeout.count());
    if (fd == shutdown_fd.Fd()) {
      running_ = false;
      return;
    }
    callback(c);
  }));
}

void Telemetry::Run() {
  running_ = true;
  AddCoroutine(
      std::make_unique<co::Coroutine>(*scheduler_, [this](co::Coroutine *c) {
        running_ = true;
        while (running_) {
          absl::Status status = HandleCommand(c);
          if (!status.ok()) {
            std::cerr << "Failed to handle telemetry command: "
                      << status.message() << std::endl;
            break;
          }
        }
      }));

  // If we are using our own scheduler, run it.  Otherwise the scheduler passed
  // to Run() will be used.
  if (scheduler_ == &local_scheduler_) {
    scheduler_->Run();
  }
}

// System telemetry module.
absl::StatusOr<std::unique_ptr<TelemetryCommand>> SystemTelemetry::ParseCommand(
    const adastra::proto::telemetry::Command &command) {
  if (command.command().Is<adastra::proto::telemetry::ShutdownCommand>()) {
    adastra::proto::telemetry::ShutdownCommand shutdown;
    if (!command.command().UnpackTo(&shutdown)) {
      return absl::InternalError("Failed to unpack shutdown command");
    }
    return std::make_unique<ShutdownCommand>(
        ShutdownCommand(shutdown.exit_code(), shutdown.timeout_seconds()));
  }
  return nullptr;
}

absl::Status
SystemTelemetry::HandleCommand(std::unique_ptr<TelemetryCommand> command,
                               co::Coroutine *c) {
  switch (command->Code()) {
  case SystemTelemetry::kShutdownCommand: {
    auto shutdown = dynamic_cast<ShutdownCommand *>(command.get());
    exit(shutdown->exit_code);
    break;
  }

  default:
    return absl::InternalError(absl::StrFormat(
        "Unknown system telemetry command code: %d", command->Code()));
  }
  return absl::OkStatus();
}

absl::Status
Telemetry::SendStatus(const adastra::proto::telemetry::Status &status,
                      co::Coroutine *c) {

  uint64_t len = status.ByteSizeLong();
  std::vector<char> buffer(len + sizeof(uint32_t));
  char *buf = buffer.data() + 4;
  if (!status.SerializeToArray(buf, uint32_t(len))) {
    return absl::InternalError("Failed to serialize telemetry status message");
  }
  // Copy length into buffer.
  memcpy(buffer.data(), &len, sizeof(uint32_t));

  // Write to pipe in a loop.
  char *sbuf = buffer.data();
  size_t remaining = len + sizeof(uint32_t);
  while (remaining > 0) {
    if (c != nullptr) {
      c->Wait(write_fd_.Fd(), POLLOUT);
    }
    ssize_t n = ::write(write_fd_.Fd(), sbuf, remaining);
    if (n <= 0) {
      return absl::InternalError("Failed to write to telemetry pipe");
    }
    remaining -= n;
    sbuf += n;
  }
  return absl::OkStatus();
}

} // namespace stagezero
