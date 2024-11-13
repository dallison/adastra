#include "stagezero/telemetry/telemetry.h"

namespace stagezero {

Telemetry::Telemetry() {
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

  // Add the system telemetry module.
  AddModule(std::make_shared<SystemTelemetry>(*this));
}

absl::StatusOr<std::unique_ptr<TelemetryCommand>>
Telemetry::GetCommand(co::Coroutine *c) const {
  // Read length.
  uint32_t len;
  if (c != nullptr) {
    c->Wait(read_fd_.Fd(), POLLIN);
  }
  ssize_t n = ::read(read_fd_.Fd(), &len, sizeof(len));
  if (n <= 0) {
    return absl::InternalError("Failed to read from telemetry pipe: " +
                               std::to_string(n) + " " + strerror(errno));
  }
  std::vector<char> buffer(len);
  char *buf = buffer.data();
  size_t remaining = len;
  while (remaining > 0) {
    if (c != nullptr) {
      c->Wait(read_fd_.Fd(), POLLIN);
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
      return std::move(*cmd);
    }
  }
  return absl::InternalError("Unknown telemetry command");
}

// System telemetry module.
absl::StatusOr<std::unique_ptr<TelemetryCommand>> SystemTelemetry::ParseCommand(
    const adastra::proto::telemetry::Command &command) {
  switch (command.code()) {
  case adastra::proto::telemetry::SHUTDOWN_COMMAND: {
    adastra::proto::telemetry::ShutdownCommand shutdown;
    if (!command.data().UnpackTo(&shutdown)) {
      return absl::InternalError("Failed to unpack shutdown command");
    }
    return std::make_unique<ShutdownCommand>(ShutdownCommand(
        shutdown.exit_code(),
        shutdown.timeout_seconds()));
  }
  case adastra::proto::telemetry::DIAGNOSTICS_COMMAND: {
    // TODO:
  }
  }
  return nullptr;
}

absl::Status SystemTelemetry::SendDiagnosticReport(const DiagnosticReport &diag,
                                                   co::Coroutine *c) {
  adastra::proto::telemetry::Status status;
  diag.ToProto(status);
  return telemetry_.SendStatus(status, c);
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
