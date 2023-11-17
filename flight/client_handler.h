#pragma once

#include "common/alarm.h"
#include "common/tcp_client_handler.h"
#include "proto/flight.pb.h"
#include "proto/capcom.pb.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "toolbelt/triggerfd.h"
#include "capcom/client/client.h"

#include "absl/container/flat_hash_map.h"
#include <list>

#include "coroutine.h"

namespace stagezero::flight {

class FlightDirector;

class ClientHandler
    : public common::TCPClientHandler<flight::Request, flight::Response,
                                      capcom::proto::Event> {
public:
  ClientHandler(FlightDirector &flight, toolbelt::TCPSocket socket)
      : TCPClientHandler(std::move(socket)), flight_(flight) {}
  ~ClientHandler();

  absl::Status SendSubsystemStatusEvent(Subsystem *subsystem);
  absl::Status SendAlarm(const capcom::Alarm &alarm);

  co::CoroutineScheduler &GetScheduler() const override;

  toolbelt::Logger &GetLogger() const override;

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) override;

private:
  std::shared_ptr<ClientHandler> shared_from_this() {
    return std::static_pointer_cast<ClientHandler>(
        TCPClientHandler<flight::Request, flight::Response,
                         capcom::proto::Event>::shared_from_this());
  }

  absl::Status HandleMessage(const flight::Request &req, flight::Response &resp,
                             co::Coroutine *c) override;

  void HandleInit(const flight::InitRequest &req,
                  flight::InitResponse *response, co::Coroutine *c);

  void HandleLoadGraph(const flight::LoadGraphRequest &req,
                       flight::LoadGraphResponse *response, co::Coroutine *c);

  void HandleStartSubsystem(const flight::StartSubsystemRequest &req,
                            flight::StartSubsystemResponse *response,
                            co::Coroutine *c);

  void HandleStopSubsystem(const flight::StopSubsystemRequest &req,
                           flight::StopSubsystemResponse *response,
                           co::Coroutine *c);
  void HandleGetSubsystems(const flight::GetSubsystemsRequest &req,
                           flight::GetSubsystemsResponse *response,
                           co::Coroutine *c);
  void HandleGetAlarms(const flight::GetAlarmsRequest &req,
                       flight::GetAlarmsResponse *response, co::Coroutine *c);

  void HandleAbort(const flight::AbortRequest &req,
                   flight::AbortResponse *response, co::Coroutine *c);

  FlightDirector &flight_;
};
} // namespace stagezero::flight
