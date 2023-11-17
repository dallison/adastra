#include "fight/client/client.h"
#include "toolbelt/hexdump.h"

namespace stagezero::flight::client {

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

  stagezero::flight::proto::Request req;
  auto init = req.mutable_init();
  init->set_client_name(name);

  stagezero::flight::proto::Response resp;
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

  std::cout << "flight connecting to event port " << event_addr.ToString()
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

  if (!event.ParseFromArray(event_buffer_, *n)) {
    event_socket_.Close();
    return absl::InternalError("Failed to parse event");
  }

  Event result;
  switch (event.event_case()) {
  case proto::Event::kSubsystemStatus: {
    const auto &s = event.subsystem_status();
    result.type = EventType::kSubsystemStatus;
    SubsystemStatusEvent status;
    status.subsystem = s.name();
    switch (s.admin_state()) {
    case flight::proto::ADMIN_OFFLINE:
      status.admin_state = capcom::AdminState::kOffline;
      break;
    case flight::proto::ADMIN_ONLINE:
      status.admin_state = capcom::AdminState::kOnline;
      break;
    default:
      return absl::InternalError(
          absl::StrFormat("Unknown admin state %d", s.admin_state()));
    }
    switch (s.oper_state()) {
    case flight::proto::OPER_OFFLINE:
      status.oper_state = capcom::OperState::kOffline;
      break;
    case flight::proto::OPER_STARTING_CHILDREN:
      status.oper_state = capcom::OperState::kStartingChildren;
      break;
    case flight::proto::OPER_STARTING_PROCESSES:
      status.oper_state = capcom::OperState::kStartingProcesses;
      break;
    case flight::proto::OPER_ONLINE:
      status.oper_state = capcom::OperState::kOnline;
      break;
    case flight::proto::OPER_STOPPING_CHILDREN:
      status.oper_state = capcom::OperState::kStoppingChildren;
      break;
    case flight::proto::OPER_STOPPING_PROCESSES:
      status.oper_state = capcom::OperState::kStoppingProcesses;
      break;
    case flight::proto::OPER_RESTARTING:
      status.oper_state = capcom::OperState::kRestarting;
      break;
    case flight::proto::OPER_BROKEN:
      status.oper_state = capcom::OperState::kBroken;
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


absl::Status Client::StartSubsystem(const std::string &name, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::flight::proto::Request req;
  auto s = req.mutable_start_subsystem();
  s->set_subsystem(name);

  stagezero::flight::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &start_resp = resp.start_subsystem();
  if (!start_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to start subsystem: %s", start_resp.error()));
  }

  if (mode_ == ClientMode::kBlocking) {
    return WaitForSubsystemState(name, capcom::AdminState::kOnline, capcom::OperState::kOnline);
  }
  return absl::OkStatus();
}

absl::Status Client::StopSubsystem(const std::string &name, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::flight::proto::Request req;
  auto s = req.mutable_stop_subsystem();
  s->set_subsystem(name);

  stagezero::flight::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &stop_resp = resp.stop_subsystem();
  if (!stop_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to start subsystem: %s", stop_resp.error()));
  }

  if (mode_ == ClientMode::kBlocking) {
    return WaitForSubsystemState(name, capcom::AdminState::kOffline,
                                 capcom::OperState::kOffline);
  }
  return absl::OkStatus();
}

absl::Status Client::WaitForSubsystemState(const std::string &subsystem,
                                           capcom::AdminState admin_state,
                                           capcom::OperState oper_state) {
  for (;;) {
    absl::StatusOr<Event> event = WaitForEvent();
    if (!event.ok()) {
      return event.status();
    }
    if (event->type == EventType::kSubsystemStatus) {
      SubsystemStatusEvent &s = std::get<0>(event->event);
      if (s.subsystem == subsystem) {
        if (admin_state == s.admin_state) {
          if (s.admin_state == capcom::AdminState::kOnline &&
              s.oper_state == capcom::OperState::kBroken) {
            return absl::InternalError(
                absl::StrFormat("Subsystem %s is broken", subsystem));
          }
          if (oper_state == s.oper_state) {
            return absl::OkStatus();
          }
        }
      }
    }
  }
}

absl::Status Client::Abort(const std::string &reason, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::flight::proto::Request req;
  req.mutable_abort()->set_reason(reason);

  stagezero::flight::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &abort_resp = resp.abort();
  if (!abort_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to abort: %s", abort_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status
Client::SendRequestReceiveResponse(const stagezero::flight::proto::Request &req,
                                   stagezero::flight::proto::Response &response,
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
} // namespace stagezero::flight::client
