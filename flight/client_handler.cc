// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "flight/client_handler.h"
#include "absl/strings/str_format.h"
#include "flight/flight_director.h"
#include "toolbelt/hexdump.h"

#include <iostream>

namespace adastra::flight {

ClientHandler::~ClientHandler() {}

co::CoroutineScheduler &ClientHandler::GetScheduler() const {
  return flight_.co_scheduler_;
}

toolbelt::Logger &ClientHandler::GetLogger() const { return flight_.logger_; }

void ClientHandler::AddCoroutine(std::unique_ptr<co::Coroutine> c) {
  flight_.AddCoroutine(std::move(c));
}

absl::Status ClientHandler::HandleMessage(const flight::proto::Request &req,
                                          flight::proto::Response &resp,
                                          co::Coroutine *c) {
  switch (req.request_case()) {
  case flight::proto::Request::kInit:
    HandleInit(req.init(), resp.mutable_init(), c);
    break;

  case flight::proto::Request::kStartSubsystem:
    HandleStartSubsystem(req.start_subsystem(), resp.mutable_start_subsystem(),
                         c);
    break;
  case flight::proto::Request::kStopSubsystem:
    HandleStopSubsystem(req.stop_subsystem(), resp.mutable_stop_subsystem(), c);
    break;
  case flight::proto::Request::kGetSubsystems:
    HandleGetSubsystems(req.get_subsystems(), resp.mutable_get_subsystems(), c);
    break;
  case flight::proto::Request::kGetAlarms:
    HandleGetAlarms(req.get_alarms(), resp.mutable_get_alarms(), c);
    break;

  case flight::proto::Request::kAbort:
    HandleAbort(req.abort(), resp.mutable_abort(), c);
    break;

  case flight::proto::Request::kInput:
    HandleInput(req.input(), resp.mutable_input(), c);
    break;

  case flight::proto::Request::kCloseFd:
    HandleCloseFd(req.close_fd(), resp.mutable_close_fd(), c);
    break;

  case flight::proto::Request::kAddGlobalVariable:
    HandleAddGlobalVariable(req.add_global_variable(),
                            resp.mutable_add_global_variable(), c);
    break;

  case flight::proto::Request::REQUEST_NOT_SET:
    return absl::InternalError("Protocol error: unknown request");
  }
  return absl::OkStatus();
}

void ClientHandler::HandleInit(const flight::proto::InitRequest &req,
                               flight::proto::InitResponse *response,
                               co::Coroutine *c) {
  absl::StatusOr<int> s = Init(req.client_name(), req.event_mask(),
                               [this] { flight_.DumpEventCache(this); }, c);
  if (!s.ok()) {
    response->set_error(s.status().ToString());
    return;
  }
  response->set_event_port(*s);
}

void ClientHandler::HandleStartSubsystem(
    const flight::proto::StartSubsystemRequest &req,
    flight::proto::StartSubsystemResponse *response, co::Coroutine *c) {
  if (!flight_.interfaces_.contains(req.subsystem())) {
    response->set_error(absl::StrFormat(
        "Cannot start non-interface subsystem %s", req.subsystem()));
    return;
  }
  Terminal terminal;
  if (req.has_interactive_terminal()) {
    terminal.FromProto(req.interactive_terminal());
  }
  if (absl::Status status = flight_.capcom_client_.StartSubsystem(
          req.subsystem(),
          req.interactive()
              ? adastra::capcom::client::RunMode::kInteractive
              : adastra::capcom::client::RunMode::kNoninteractive,
          terminal.IsPresent() ? &terminal : nullptr, c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleStopSubsystem(
    const flight::proto::StopSubsystemRequest &req,
    flight::proto::StopSubsystemResponse *response, co::Coroutine *c) {
  if (!flight_.interfaces_.contains(req.subsystem())) {
    response->set_error(absl::StrFormat(
        "Cannot stop non-interface subsystem %s", req.subsystem()));
    return;
  }
  if (absl::Status status =
          flight_.capcom_client_.StopSubsystem(req.subsystem(), c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleGetSubsystems(
    const flight::proto::GetSubsystemsRequest &req,
    flight::proto::GetSubsystemsResponse *response, co::Coroutine *c) {
  absl::StatusOr<std::vector<SubsystemStatus>> status =
      flight_.capcom_client_.GetSubsystems(c);
  if (!status.ok()) {
    response->set_error(status.status().ToString());
    return;
  }

  for (auto &st : *status) {
    auto *s = response->add_subsystems();
    st.ToProto(s);
  }
}

void ClientHandler::HandleGetAlarms(const flight::proto::GetAlarmsRequest &req,
                                    flight::proto::GetAlarmsResponse *response,
                                    co::Coroutine *c) {
  absl::StatusOr<std::vector<Alarm>> alarms =
      flight_.capcom_client_.GetAlarms(c);
  if (!alarms.ok()) {
    response->set_error(alarms.status().ToString());
  }
  for (auto &alarm : *alarms) {
    auto *a = response->add_alarms();
    alarm.ToProto(a);
  }
}

void ClientHandler::HandleAbort(const flight::proto::AbortRequest &req,
                                flight::proto::AbortResponse *response,
                                co::Coroutine *c) {
  if (absl::Status status = flight_.capcom_client_.Abort(req.reason(), req.emergency(), c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleAddGlobalVariable(
    const proto::AddGlobalVariableRequest &req,
    proto::AddGlobalVariableResponse *response, co::Coroutine *c) {
  Variable var = {.name = req.var().name(),
                  .value = req.var().value(),
                  .exported = req.var().exported()};
  if (absl::Status status =
          flight_.capcom_client_.AddGlobalVariable(std::move(var), c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleInput(const proto::InputRequest &req,
                                proto::InputResponse *response,
                                co::Coroutine *c) {
  Subsystem *subsystem = flight_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  Process *proc = flight_.FindInteractiveProcess(subsystem);
  if (proc == nullptr) {
    response->set_error(absl::StrFormat(
        "Subsystem %s doesn't have an interactive process", req.subsystem()));
    return;
  }
  if (absl::Status status = flight_.capcom_client_.SendInput(
          req.subsystem(), proc->name, req.fd(), req.data());
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleCloseFd(const proto::CloseFdRequest &req,
                                  proto::CloseFdResponse *response,
                                  co::Coroutine *c) {
  Subsystem *subsystem = flight_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  Process *proc = flight_.FindInteractiveProcess(subsystem);
  if (proc == nullptr) {
    response->set_error(absl::StrFormat(
        "Subsystem %s doesn't have an interactive procdess", req.subsystem()));
    return;
  }
  if (absl::Status status =
          flight_.capcom_client_.CloseFd(req.subsystem(), proc->name, req.fd());
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

absl::Status
ClientHandler::SendEvent(std::shared_ptr<adastra::Event> event) {
  if (!event->IsMaskedIn(event_mask_)) {
    return absl::OkStatus();
  }
  auto proto_event = std::make_shared<adastra::proto::Event>();
  event->ToProto(proto_event.get());
  return QueueEvent(std::move(proto_event));
}
} // namespace adastra::flight
