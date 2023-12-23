// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "flight/client/client.h"
#include "toolbelt/hexdump.h"

namespace stagezero::flight::client {

absl::Status Client::Init(toolbelt::InetAddress addr, const std::string &name,
int event_mask,
                          co::Coroutine *co) {
  auto fill_init = [name, event_mask](flight::proto::Request &req) {
    auto init = req.mutable_init();
    init->set_client_name(name);
    init->set_event_mask(event_mask);
  };

  auto parse_init =
      [](const flight::proto::Response &resp) -> absl::StatusOr<int> {
    auto init_resp = resp.init();
    if (!init_resp.error().empty()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to initialize client connection: %s", init_resp.error()));
    }
    return init_resp.event_port();
  };

  return TCPClient::Init(addr, name, fill_init, parse_init, co);
}

absl::StatusOr<std::shared_ptr<Event>> Client::ReadEvent(co::Coroutine *c) {
  absl::StatusOr<std::shared_ptr<stagezero::proto::Event>> proto_event =
      ReadProtoEvent(c);
  if (!proto_event.ok()) {
    return proto_event.status();
  }
  auto result = std::make_shared<Event>();
  if (absl::Status status = result->FromProto(**proto_event); !status.ok()) {
    return status;
  }
  return result;
}

absl::Status Client::StartSubsystem(const std::string &name, RunMode mode,
                                    Terminal *terminal, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  if (mode == RunMode::kInteractive && mode_ == ClientMode::kBlocking) {
    return absl::InternalError(
        "Using interactive with a blocking client is "
        "racy as both the client and the caller will be processing events");
  }
  stagezero::flight::proto::Request req;
  auto s = req.mutable_start_subsystem();
  s->set_subsystem(name);
  s->set_interactive(mode == RunMode::kInteractive);
  if (terminal != nullptr) {
    terminal->ToProto(s->mutable_interactive_terminal());
  }
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
    return WaitForSubsystemState(name, AdminState::kOnline, OperState::kOnline,
                                 c);
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
    return WaitForSubsystemState(name, AdminState::kOffline,
                                 OperState::kOffline, c);
  }

  return absl::OkStatus();
}

absl::Status Client::WaitForSubsystemState(const std::string &subsystem,
                                           AdminState admin_state,
                                           OperState oper_state,
                                           co::Coroutine *c) {
  for (;;) {
    absl::StatusOr<std::shared_ptr<Event>> e = WaitForEvent(c);
    if (!e.ok()) {
      return e.status();
    }
    std::shared_ptr<Event> event = *e;
    if (event->type == EventType::kSubsystemStatus) {
      SubsystemStatus &s = std::get<0>(event->event);
      if (s.subsystem == subsystem) {
        if (admin_state == s.admin_state) {
          if (s.admin_state == AdminState::kOnline &&
              s.oper_state == OperState::kBroken) {
            return absl::InternalError(
                absl::StrFormat("Subsystem %s is broken", subsystem));
          }
          if (oper_state == s.oper_state) {
            return absl::OkStatus();
          }
        }
      }
    } else if (event->type == EventType::kOutput) {
      auto &output = std::get<2>(event->event);
      ::write(output.fd, output.data.data(), output.data.size());
    }
  }
}

absl::Status Client::Abort(const std::string &reason, bool emergency, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::flight::proto::Request req;
  auto abort = req.mutable_abort();
  abort->set_reason(reason);
  abort->set_emergency(emergency);

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

absl::StatusOr<std::vector<SubsystemStatus>>
Client::GetSubsystems(co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::flight::proto::Request req;
  (void)req.mutable_get_subsystems();

  stagezero::flight::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &get_resp = resp.get_subsystems();
  if (!get_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to get subsystems: %s", get_resp.error()));
  }

  std::vector<SubsystemStatus> result(get_resp.subsystems_size());
  int index = 0;
  for (auto &status : get_resp.subsystems()) {
    if (absl::Status s = result[index].FromProto(status); !s.ok()) {
      return s;
    }
    index++;
  }
  return result;
}

absl::StatusOr<std::vector<Alarm>> Client::GetAlarms(co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::flight::proto::Request req;
  (void)req.mutable_get_alarms();

  stagezero::flight::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &get_resp = resp.get_alarms();
  if (!get_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to get alarms: %s", get_resp.error()));
  }

  std::vector<Alarm> result(get_resp.alarms_size());
  int index = 0;
  for (auto &status : get_resp.alarms()) {
    result[index].FromProto(status);
    index++;
  }
  return result;
}

absl::Status Client::SendInput(const std::string &subsystem, int fd,
                               const std::string &data, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::flight::proto::Request req;
  auto input = req.mutable_input();
  input->set_subsystem(subsystem);
  input->set_fd(fd);
  input->set_data(data);

  stagezero::flight::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &input_resp = resp.input();
  if (!input_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to abort: %s", input_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::AddGlobalVariable(const Variable &var, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::flight::proto::Request req;
  auto *v = req.mutable_add_global_variable()->mutable_var();
  v->set_name(var.name);
  v->set_value(var.value);
  v->set_exported(var.exported);

  stagezero::flight::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &var_resp = resp.add_global_variable();
  if (!var_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to add global variable: %s", var_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::CloseFd(const std::string &subsystem, int fd,
                             co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  stagezero::flight::proto::Request req;
  auto close = req.mutable_close_fd();
  close->set_subsystem(subsystem);
  close->set_fd(fd);

  stagezero::flight::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &close_resp = resp.close_fd();
  if (!close_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to close fd: %s", close_resp.error()));
  }
  return absl::OkStatus();
}
} // namespace stagezero::flight::client
