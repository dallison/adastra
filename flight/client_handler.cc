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

absl::Status ClientHandler::HandleMessage(const flight::Request &req,
                                          flight::Response &resp,
                                          co::Coroutine *c) {
  switch (req.request_case()) {
  case flight::Request::kInit:
    HandleInit(req.init(), resp.mutable_init(), c);
    break;

  case flight::Request::kLoadGraph:
    HandleLoadGraph(req.load_graph(), resp.mutable_load_graph(), c);
    break;
  case flight::Request::kStartSubsystem:
    HandleStartSubsystem(req.start_subsystem(), resp.mutable_start_subsystem(),
                         c);
    break;
  case flight::Request::kStopSubsystem:
    HandleStopSubsystem(req.stop_subsystem(), resp.mutable_stop_subsystem(), c);
    break;
  case flight::Request::kGetSubsystems:
    HandleGetSubsystems(req.get_subsystems(), resp.mutable_get_subsystems(), c);
    break;
  case flight::Request::kGetAlarms:
    HandleGetAlarms(req.get_alarms(), resp.mutable_get_alarms(), c);
    break;

  case flight::Request::kAbort:
    HandleAbort(req.abort(), resp.mutable_abort(), c);
    break;

  case flight::Request::REQUEST_NOT_SET:
    return absl::InternalError("Protocol error: unknown request");
  }
  return absl::OkStatus();
}

void ClientHandler::HandleInit(const flight::InitRequest &req,
                               flight::InitResponse *response,
                               co::Coroutine *c) {
  absl::StatusOr<int> s = Init(req.client_name(), c);
  if (!s.ok()) {
    response->set_error(s.status().ToString());
    return;
  }
  response->set_event_port(*s);
}

void ClientHandler::HandleLoadGraph(const flight::LoadGraphRequest &req,
                                    flight::LoadGraphResponse *response,
                                    co::Coroutine *c) {}

void ClientHandler::HandleStartSubsystem(
    const flight::StartSubsystemRequest &req,
    flight::StartSubsystemResponse *response, co::Coroutine *c) {}

void ClientHandler::HandleStopSubsystem(const flight::StopSubsystemRequest &req,
                                        flight::StopSubsystemResponse *response,
                                        co::Coroutine *c) {}

void ClientHandler::HandleGetSubsystems(const flight::GetSubsystemsRequest &req,
                                        flight::GetSubsystemsResponse *response,
                                        co::Coroutine *c) {}

void ClientHandler::HandleGetAlarms(const flight::GetAlarmsRequest &req,
                                    flight::GetAlarmsResponse *response,
                                    co::Coroutine *c) {}

void ClientHandler::HandleAbort(const flight::AbortRequest &req,
                                flight::AbortResponse *response,
                                co::Coroutine *c) {}

} // namespace stagezero::flight
