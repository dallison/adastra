#include "stagezero/parameters/parameters.h"
#include "toolbelt/hexdump.h"

namespace stagezero {
Parameters::Parameters(bool events) {
  // Look for the STAGEZERO_PARAMETERS_FD environment variable.
  const char *env = getenv("STAGEZERO_PARAMETERS_FDS");
  if (env == nullptr) {
    // No parameters stream.
    return;
  }
  int rfd, wfd, efd;
  sscanf(env, "%d:%d:%d", &rfd, &wfd, &efd);
  read_fd_.SetFd(rfd);
  write_fd_.SetFd(wfd);
  event_fd_.SetFd(efd);

  // Send the init message.  This tells stagezero if we want events or not.
  adastra::proto::parameters::Request req;
  adastra::proto::parameters::Response resp;
  req.mutable_init()->set_events(events);
  if (absl::Status status = SendRequestReceiveResponse(req, resp);
      !status.ok()) {
    read_fd_.Close();
    write_fd_.Close();
    event_fd_.Close();
  }
}

absl::Status Parameters::SetParameter(const std::string &name,
                                      adastra::parameters::Value value) {
  if (!IsOpen()) {
    return absl::InternalError("No parameters stream");
  }
  adastra::proto::parameters::Request req;
  req.mutable_set_parameter()->mutable_parameter()->set_name(name);
  value.ToProto(
      req.mutable_set_parameter()->mutable_parameter()->mutable_value());
  adastra::proto::parameters::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp);
      !status.ok()) {
    return status;
  }

  if (!resp.set_parameter().error().empty()) {
    return absl::InternalError("Failed to set parameter: " +
                               resp.set_parameter().error());
  }
  return absl::OkStatus();
}

absl::Status Parameters::DeleteParameter(const std::string &name) {
  if (!IsOpen()) {
    return absl::InternalError("No parameters stream");
  }
  adastra::proto::parameters::Request req;
  req.mutable_delete_parameter()->set_name(name);
  adastra::proto::parameters::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp);
      !status.ok()) {
    return status;
  }

  if (!resp.delete_parameter().error().empty()) {
    return absl::InternalError("Failed to delete parameter: " +
                               resp.delete_parameter().error());
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<std::string>> Parameters::ListParameters() {
  if (!IsOpen()) {
    return absl::InternalError("No parameters stream");
  }
  adastra::proto::parameters::Request req;
  req.mutable_list_parameters();
  adastra::proto::parameters::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp);
      !status.ok()) {
    return status;
  }

  if (!resp.list_parameters().error().empty()) {
    return absl::InternalError("Failed to list parameters: " +
                               resp.list_parameters().error());
  }
  std::vector<std::string> names;
  for (const auto &name : resp.list_parameters().names()) {
    names.push_back(name);
  }
  return names;
}

absl::StatusOr<std::vector<std::shared_ptr<adastra::parameters::ParameterNode>>>
Parameters::GetAllParameters() {
  if (!IsOpen()) {
    return absl::InternalError("No parameters stream");
  }
  adastra::proto::parameters::Request req;
  req.mutable_get_all_parameters();
  adastra::proto::parameters::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp);
      !status.ok()) {
    return status;
  }

  if (!resp.get_all_parameters().error().empty()) {
    return absl::InternalError("Failed to get all parameters: " +
                               resp.get_all_parameters().error());
  }
  std::vector<std::shared_ptr<adastra::parameters::ParameterNode>> params;
  for (const auto &param : resp.get_all_parameters().parameters()) {
    auto p = std::make_shared<adastra::parameters::ParameterNode>();
    p->FromProto(param);
    params.push_back(p);
  }
  return params;
}

absl::StatusOr<adastra::parameters::Value>
Parameters::GetParameter(const std::string &name) {
  if (!IsOpen()) {
    return absl::InternalError("No parameters stream");
  }
  adastra::proto::parameters::Request req;
  req.mutable_get_parameter()->set_name(name);
  adastra::proto::parameters::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp);
      !status.ok()) {
    return status;
  }

  if (!resp.get_parameter().error().empty()) {
    return absl::InternalError("Failed to get parameter: " +
                               resp.get_parameter().error());
  }
  adastra::parameters::Value value;
  value.FromProto(resp.get_parameter().value());
  return value;
}

absl::StatusOr<bool> Parameters::HasParameter(const std::string &name) {
  if (!IsOpen()) {
    return absl::InternalError("No parameters stream");
  }
  adastra::proto::parameters::Request req;
  req.mutable_has_parameter()->set_name(name);
  adastra::proto::parameters::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp);
      !status.ok()) {
    return status;
  }

  if (!resp.has_parameter().error().empty()) {
    return absl::InternalError("Failed to check for parameter: " +
                               resp.has_parameter().error());
  }
  return resp.has_parameter().has();
}

std::unique_ptr<adastra::parameters::ParameterEvent>
Parameters::GetEvent() const {
  std::unique_ptr<adastra::parameters::ParameterEvent> event;

  if (!event_fd_.Valid()) {
    return event;
  }
  uint32_t len;
  ssize_t n = ::read(event_fd_.Fd(), &len, sizeof(len));
  if (n <= 0) {
    return event;
  }
  std::cerr << "Got event of length " << len << std::endl;
  std::vector<char> buffer(len);
  char *buf = buffer.data();
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = ::read(event_fd_.Fd(), buf, remaining);
    if (n <= 0) {
      return event;
    }
    remaining -= n;
    buf += n;
  }
  adastra::proto::parameters::ParameterEvent event_proto;
  if (!event_proto.ParseFromArray(buffer.data(), buffer.size())) {
    return event;
  }
  switch (event_proto.event_case()) {
  case adastra::proto::parameters::ParameterEvent::kUpdate: {
    auto update = std::make_unique<adastra::parameters::ParameterUpdateEvent>();
    update->name = event_proto.update().name();
    update->value.FromProto(event_proto.update().value());
    event = std::move(update);
    break;
  }
  case adastra::proto::parameters::ParameterEvent::kDelete: {
    auto del = std::make_unique<adastra::parameters::ParameterDeleteEvent>();
    del->name = event_proto.delete_();
    event = std::move(del);
    break;
  }
  default:
    // Ignore invalid event.
    break;
  }
  return event;
}

absl::Status Parameters::SendRequestReceiveResponse(
    const adastra::proto::parameters::Request &req,
    adastra::proto::parameters::Response &resp) {
  {
    uint64_t len = req.ByteSizeLong();
    std::vector<char> buffer(len + sizeof(uint32_t));
    char *buf = buffer.data() + 4;
    if (!req.SerializeToArray(buf, uint32_t(len))) {
      return absl::InternalError("Failed to serialize request message");
    }
    // Copy length into buffer.
    memcpy(buffer.data(), &len, sizeof(uint32_t));

    // Write to pipe in a loop.
    char *sbuf = buffer.data();
    size_t remaining = len + sizeof(uint32_t);
    while (remaining > 0) {
      ssize_t n = ::write(write_fd_.Fd(), sbuf, remaining);
      if (n <= 0) {
        return absl::InternalError("Failed to write to parameters pipe");
      }
      remaining -= n;
      sbuf += n;
    }
  }
  // Read length.
  uint32_t len;
  ssize_t n = ::read(read_fd_.Fd(), &len, sizeof(len));
  if (n <= 0) {
    return absl::InternalError("Failed to read from parameters pipe: " +
                               std::to_string(n) + " " + strerror(errno));
  }
  std::vector<char> buffer(len);
  char *buf = buffer.data();
  size_t remaining = len;
  while (remaining > 0) {
    ssize_t n = ::read(read_fd_.Fd(), buf, remaining);
    if (n <= 0) {
      return absl::InternalError("Failed to read from parameters pipe");
    }
    remaining -= n;
    buf += n;
  }

  if (!resp.ParseFromArray(buffer.data(), buffer.size())) {
    return absl::InternalError("Failed to parse response message");
  }

  return absl::OkStatus();
}
} // namespace stagezero