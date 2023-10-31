#include "capcom/client/client.h"
#include "toolbelt/hexdump.h"

namespace stagezero::capcom::client {

absl::Status Client::Init(toolbelt::InetAddress addr, const std::string &name) {
  absl::Status status = command_socket_.Connect(addr);
  if (!status.ok()) {
    return status;
  }

  if (absl::Status status = command_socket_.SetCloseOnExec(); !status.ok()) {
    return status;
  }

  name_ = name;

  stagezero::capcom::proto::Request req;
  auto init = req.mutable_init();
  init->set_client_name(name);

  stagezero::capcom::proto::Response resp;
  status = SendRequestReceiveResponse(req, resp);
  if (!status.ok()) {
    return status;
  }

  auto init_resp = resp.init();
  if (!init_resp.error().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to initialize client connection: %s", init_resp.error()));
  }

  return absl::OkStatus();
}

absl::Status Client::AddSubsystem(const std::string &name,
                                  const SubsystemOptions &options) {
  stagezero::capcom::proto::Request req;
  auto add = req.mutable_add_subsystem();
  add->set_name(name);

  // Add all static processes to the proto message.
  for (auto &sproc : options.static_processes) {
    auto *proc = add->add_processes();
    auto *opts = proc->mutable_options();
    opts->set_name(sproc.name);
    opts->set_description(sproc.description);
    opts->set_sigint_shutdown_timeout_secs(sproc.sigint_shutdown_timeout_secs);
    opts->set_sigterm_shutdown_timeout_secs(
        sproc.sigterm_shutdown_timeout_secs);
    auto *s = proc->mutable_static_process();
    s->set_executable(sproc.executable);
  }

  stagezero::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp);
  if (!status.ok()) {
    return status;
  }

  auto& add_resp = resp.add_subsystem();
  if (!add_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to add subsystem: %s", add_resp.error()));
  }

  return absl::OkStatus();
}

absl::Status Client::RemoveSubsystem(const std::string &name, bool recursive) {
  stagezero::capcom::proto::Request req;
  auto r = req.mutable_remove_subsystem();
  r->set_subsystem(name);
  r->set_recursive(recursive);

  stagezero::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp);
  if (!status.ok()) {
    return status;
  }

  auto& remove_resp = resp.remove_subsystem();
  if (!remove_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to remove subsystem: %s", remove_resp.error()));
  }

  return absl::OkStatus();
}

absl::Status Client::StartSubsystem(const std::string &name) {
  stagezero::capcom::proto::Request req;
  auto s = req.mutable_start_subsystem();
  s->set_subsystem(name);

  stagezero::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp);
  if (!status.ok()) {
    return status;
  }

  auto& start_resp = resp.start_subsystem();
  if (!start_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to start subsystem: %s", start_resp.error()));
  }

  return absl::OkStatus();
}

absl::Status Client::StopSubsystem(const std::string &name) {
  stagezero::capcom::proto::Request req;
  auto s = req.mutable_stop_subsystem();
  s->set_subsystem(name);

  stagezero::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp);
  if (!status.ok()) {
    return status;
  }

  auto& stop_resp = resp.stop_subsystem();
  if (!stop_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to start subsystem: %s", stop_resp.error()));
  }

  return absl::OkStatus();
}

absl::Status Client::SendRequestReceiveResponse(
    const stagezero::capcom::proto::Request &req,
    stagezero::capcom::proto::Response &response) {
  // SendMessage needs 4 bytes before the buffer passed to
  // use for the length.
  char *sendbuf = command_buffer_ + sizeof(int32_t);
  constexpr size_t kSendBufLen = sizeof(command_buffer_) - sizeof(int32_t);

  if (!req.SerializeToArray(sendbuf, kSendBufLen)) {
    return absl::InternalError("Failed to serialize request");
  }

  size_t length = req.ByteSizeLong();

  std::cout << "CLIENT SEND\n";
  toolbelt::Hexdump(sendbuf, length);
  absl::StatusOr<ssize_t> n = command_socket_.SendMessage(sendbuf, length, co_);
  if (!n.ok()) {
    command_socket_.Close();
    return n.status();
  }

  // Wait for response and put it in the same buffer we used for send.
  n = command_socket_.ReceiveMessage(command_buffer_, sizeof(command_buffer_),
                                     co_);
  if (!n.ok()) {
    command_socket_.Close();
    return n.status();
  }
  std::cout << "CLIENT RECV\n";
  toolbelt::Hexdump(command_buffer_, *n);

  if (!response.ParseFromArray(command_buffer_, *n)) {
    command_socket_.Close();
    return absl::InternalError("Failed to parse response");
  }

  return absl::OkStatus();
}
} // namespace stagezero::capcom::client
