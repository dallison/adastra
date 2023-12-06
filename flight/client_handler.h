// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "capcom/client/client.h"
#include "common/alarm.h"
#include "common/tcp_client_handler.h"
#include "flight/subsystem.h"
#include "proto/capcom.pb.h"
#include "proto/flight.pb.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "toolbelt/triggerfd.h"

#include <list>
#include "absl/container/flat_hash_map.h"

#include "coroutine.h"

namespace stagezero::flight {

class FlightDirector;

class ClientHandler : public common::TCPClientHandler<flight::proto::Request,
                                                      flight::proto::Response,
                                                      stagezero::proto::Event> {
 public:
  ClientHandler(FlightDirector &flight, toolbelt::TCPSocket socket)
      : TCPClientHandler(std::move(socket)), flight_(flight) {}
  ~ClientHandler();

  absl::Status SendSubsystemStatusEvent(Subsystem *subsystem);
  absl::Status SendAlarm(const Alarm &alarm);

  co::CoroutineScheduler &GetScheduler() const override;

  toolbelt::Logger &GetLogger() const override;

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) override;

 private:
  std::shared_ptr<ClientHandler> shared_from_this() {
    return std::static_pointer_cast<ClientHandler>(
        TCPClientHandler<flight::proto::Request, flight::proto::Response,
                         stagezero::proto::Event>::shared_from_this());
  }

  absl::Status HandleMessage(const flight::proto::Request &req,
                             flight::proto::Response &resp,
                             co::Coroutine *c) override;

  void HandleInit(const flight::proto::InitRequest &req,
                  flight::proto::InitResponse *response, co::Coroutine *c);

  void HandleStartSubsystem(const flight::proto::StartSubsystemRequest &req,
                            flight::proto::StartSubsystemResponse *response,
                            co::Coroutine *c);

  void HandleStopSubsystem(const flight::proto::StopSubsystemRequest &req,
                           flight::proto::StopSubsystemResponse *response,
                           co::Coroutine *c);
  void HandleGetSubsystems(const flight::proto::GetSubsystemsRequest &req,
                           flight::proto::GetSubsystemsResponse *response,
                           co::Coroutine *c);
  void HandleGetAlarms(const flight::proto::GetAlarmsRequest &req,
                       flight::proto::GetAlarmsResponse *response,
                       co::Coroutine *c);

  void HandleAbort(const flight::proto::AbortRequest &req,
                   flight::proto::AbortResponse *response, co::Coroutine *c);

  void HandleAddGlobalVariable(const flight::proto::AddGlobalVariableRequest &req,
                               flight::proto::AddGlobalVariableResponse *response,
                               co::Coroutine *c);

  void HandleInput(const flight::proto::InputRequest &req,
                   flight::proto::InputResponse *response, co::Coroutine *c);
  
    void HandleCloseFd(const flight::proto::CloseFdRequest &req,
                   flight::proto::CloseFdResponse *response, co::Coroutine *c);

  FlightDirector &flight_;
};
}  // namespace stagezero::flight
