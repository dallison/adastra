#include "capcom/client/client.h"
#include "toolbelt/hexdump.h"

namespace stagezero::capcom::client {

absl::Status Client::Init(toolbelt::InetAddress addr, const std::string &name,
                          co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
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
  status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto init_resp = resp.init();
  if (!init_resp.error().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to initialize client connection: %s", init_resp.error()));
  }

  toolbelt::InetAddress event_addr = addr;
  event_addr.SetPort(init_resp.event_port());

  std::cout << "capcom connecting to event port " << event_addr.ToString()
            << std::endl;

  if (absl::Status status = event_socket_.Connect(event_addr); !status.ok()) {
    return status;
  }

  if (absl::Status status = event_socket_.SetCloseOnExec(); !status.ok()) {
    return status;
  }

  return absl::OkStatus();
}

absl::StatusOr<Event> Client::ReadEvent(co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  proto::Event event;

  absl::StatusOr<ssize_t> n =
      event_socket_.ReceiveMessage(event_buffer_, sizeof(event_buffer_), c);
  if (!n.ok()) {
    event_socket_.Close();
    return n.status();
  }
  std::cerr << "CAPCOM EVENT\n";
  toolbelt::Hexdump(event_buffer_, *n);
  if (!event.ParseFromArray(event_buffer_, *n)) {
    event_socket_.Close();
    return absl::InternalError("Failed to parse event");
  }
  std::cerr << event.DebugString();

  Event result;
  switch (event.event_case()) {
  case proto::Event::kSubsystemStatus: {
    const auto &s = event.subsystem_status();
    result.type = EventType::kSubsystemStatus;
    SubsystemStatusEvent status;
    status.subsystem = s.name();
    switch (s.admin_state()) {
    case capcom::proto::ADMIN_OFFLINE:
      status.admin_state = AdminState::kOffline;
      break;
    case capcom::proto::ADMIN_ONLINE:
      status.admin_state = AdminState::kOnline;
      break;
    default:
      return absl::InternalError(
          absl::StrFormat("Unknown admin state %d", s.admin_state()));
    }
    switch (s.oper_state()) {
    case capcom::proto::OPER_OFFLINE:
      status.oper_state = OperState::kOffline;
      break;
    case capcom::proto::OPER_STARTING_CHILDREN:
      status.oper_state = OperState::kStartingChildren;
      break;
    case capcom::proto::OPER_STARTING_PROCESSES:
      status.oper_state = OperState::kStartingProcesses;
      break;
    case capcom::proto::OPER_ONLINE:
      status.oper_state = OperState::kOnline;
      break;
    case capcom::proto::OPER_STOPPING_CHILDREN:
      status.oper_state = OperState::kStoppingChildren;
      break;
    case capcom::proto::OPER_STOPPING_PROCESSES:
      status.oper_state = OperState::kStoppingProcesses;
      break;
    case capcom::proto::OPER_RESTARTING:
      status.oper_state = OperState::kRestarting;
      break;
    case capcom::proto::OPER_BROKEN:
      status.oper_state = OperState::kBroken;
      break;
    default:
      return absl::InternalError(
          absl::StrFormat("Unknown oper state %d", s.oper_state()));
    }

    // Add the processes status.
    for (auto &proc : s.processes()) {
      status.processes.push_back({.name = proc.name(),
                                  .process_id = proc.process_id(),
                                  .pid = proc.pid(),
                                  .running = proc.running()});
    }
    result.event = status;
    break;
  }

  case proto::Event::kAlarm: {
    Alarm alarm;
    alarm.FromProto(event.alarm());
    result.event = alarm;
    break;
  }
  default:
    // Unknown event type.
    return absl::InternalError(
        absl::StrFormat("Unknown event type %d", event.event_case()));
  }
  return result;
}

absl::Status Client::AddSubsystem(const std::string &name,
                                  const SubsystemOptions &options,
                                  co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
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

  for (auto &child : options.children) {
    auto *ch = add->add_children();
    *ch = child;
  }

  stagezero::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &add_resp = resp.add_subsystem();
  if (!add_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to add subsystem: %s", add_resp.error()));
  }

  return absl::OkStatus();
}

absl::Status Client::RemoveSubsystem(const std::string &name, bool recursive,
                                     co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::capcom::proto::Request req;
  auto r = req.mutable_remove_subsystem();
  r->set_subsystem(name);
  r->set_recursive(recursive);

  stagezero::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &remove_resp = resp.remove_subsystem();
  if (!remove_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to remove subsystem: %s", remove_resp.error()));
  }

  return absl::OkStatus();
}

absl::Status Client::StartSubsystem(const std::string &name, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::capcom::proto::Request req;
  auto s = req.mutable_start_subsystem();
  s->set_subsystem(name);

  stagezero::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &start_resp = resp.start_subsystem();
  if (!start_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to start subsystem: %s", start_resp.error()));
  }

  return absl::OkStatus();
}

absl::Status Client::StopSubsystem(const std::string &name, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::capcom::proto::Request req;
  auto s = req.mutable_stop_subsystem();
  s->set_subsystem(name);

  stagezero::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &stop_resp = resp.stop_subsystem();
  if (!stop_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to start subsystem: %s", stop_resp.error()));
  }

  return absl::OkStatus();
}

absl::Status
Client::SendRequestReceiveResponse(const stagezero::capcom::proto::Request &req,
                                   stagezero::capcom::proto::Response &response,
                                   co::Coroutine *c) {
  // SendMessage needs 4 bytes before the buffer passed to
  // use for the length.
  char *sendbuf = command_buffer_ + sizeof(int32_t);
  constexpr size_t kSendBufLen = sizeof(command_buffer_) - sizeof(int32_t);

  if (!req.SerializeToArray(sendbuf, kSendBufLen)) {
    return absl::InternalError("Failed to serialize request");
  }

  size_t length = req.ByteSizeLong();
  absl::StatusOr<ssize_t> n = command_socket_.SendMessage(sendbuf, length, c);
  if (!n.ok()) {
    command_socket_.Close();
    return n.status();
  }

  // Wait for response and put it in the same buffer we used for send.
  n = command_socket_.ReceiveMessage(command_buffer_, sizeof(command_buffer_),
                                     c);
  if (!n.ok()) {
    command_socket_.Close();
    return n.status();
  }

  if (!response.ParseFromArray(command_buffer_, *n)) {
    command_socket_.Close();
    return absl::InternalError("Failed to parse response");
  }

  return absl::OkStatus();
}
} // namespace stagezero::capcom::client
