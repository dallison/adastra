// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "flight/client_handler.h"
#include "absl/strings/str_format.h"
#include "flight/flight_director.h"
#include "toolbelt/hexdump.h"

#include <iostream>

namespace stagezero::flight {

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

  case flight::proto::Request::REQUEST_NOT_SET:
    return absl::InternalError("Protocol error: unknown request");
  }
  return absl::OkStatus();
}

void ClientHandler::HandleInit(const flight::proto::InitRequest &req,
                               flight::proto::InitResponse *response,
                               co::Coroutine *c) {
  absl::StatusOr<int> s = Init(req.client_name(), c);
  if (!s.ok()) {
    response->set_error(s.status().ToString());
    return;
  }
  response->set_event_port(*s);
}

void ClientHandler::HandleStartSubsystem(
    const flight::proto::StartSubsystemRequest &req,
    flight::proto::StartSubsystemResponse *response, co::Coroutine *c) {
  if (absl::Status status =
          flight_.capcom_client_.StartSubsystem(req.subsystem(), c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleStopSubsystem(
    const flight::proto::StopSubsystemRequest &req,
    flight::proto::StopSubsystemResponse *response, co::Coroutine *c) {
  if (absl::Status status =
          flight_.capcom_client_.StopSubsystem(req.subsystem(), c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleGetSubsystems(
    const flight::proto::GetSubsystemsRequest &req,
    flight::proto::GetSubsystemsResponse *response, co::Coroutine *c) {}

void ClientHandler::HandleGetAlarms(const flight::proto::GetAlarmsRequest &req,
                                    flight::proto::GetAlarmsResponse *response,
                                    co::Coroutine *c) {}

void ClientHandler::HandleAbort(const flight::proto::AbortRequest &req,
                                flight::proto::AbortResponse *response,
                                co::Coroutine *c) {}

} // namespace stagezero::flight
