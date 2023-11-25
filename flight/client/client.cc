// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "flight/client/client.h"
#include "toolbelt/hexdump.h"

namespace stagezero::flight::client {

absl::Status Client::Init(toolbelt::InetAddress addr, const std::string &name,
                          co::Coroutine *co) {

  auto fill_init = [name](flight::proto::Request &req) {
    auto init = req.mutable_init();
    init->set_client_name(name);
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
    return WaitForSubsystemState(name, AdminState::kOnline, OperState::kOnline, c);
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
                                           OperState oper_state, co::Coroutine* c) {
  for (;;) {
    absl::StatusOr<std::shared_ptr<Event>> e = WaitForEvent(c);
    if (!e.ok()) {
      return e.status();
    }
    std::shared_ptr<Event> event = *e;
    if (event->type == EventType::kSubsystemStatus) {
      SubsystemStatusEvent &s = std::get<0>(event->event);
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
} // namespace stagezero::flight::client
