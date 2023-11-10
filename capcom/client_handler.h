#pragma once

#include "capcom/subsystem.h"
#include "common/alarm.h"
#include "common/tcp_client_handler.h"
#include "proto/capcom.pb.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "toolbelt/triggerfd.h"

#include "absl/container/flat_hash_map.h"
#include <list>

#include "coroutine.h"

namespace stagezero::capcom {

class Capcom;

class ClientHandler
    : public common::TCPClientHandler<proto::Request, proto::Response,
                                      proto::Event> {
public:
  ClientHandler(Capcom &capcom, toolbelt::TCPSocket socket, uint32_t id)
      : TCPClientHandler(std::move(socket)), capcom_(capcom), id_(id) {}
  ~ClientHandler();

  absl::Status SendSubsystemStatusEvent(Subsystem *subsystem);
  absl::Status SendAlarm(const Alarm &alarm);

  co::CoroutineScheduler &GetScheduler() const override;

  toolbelt::Logger &GetLogger() const override;

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) override;

private:
  std::shared_ptr<ClientHandler> shared_from_this() {
    return std::static_pointer_cast<ClientHandler>(
        TCPClientHandler<proto::Request, proto::Response,
                         proto::Event>::shared_from_this());
  }

  absl::Status HandleMessage(const proto::Request &req, proto::Response &resp,
                             co::Coroutine *c) override;

  void HandleInit(const proto::InitRequest &req, proto::InitResponse *response,
                  co::Coroutine *c);

  void HandleAddCompute(const proto::AddComputeRequest &req,
                        proto::AddComputeResponse *response, co::Coroutine *c);

  void HandleRemoveCompute(const proto::RemoveComputeRequest &req,
                           proto::RemoveComputeResponse *response,
                           co::Coroutine *c);

  void HandleAddSubsystem(const proto::AddSubsystemRequest &req,
                          proto::AddSubsystemResponse *response,
                          co::Coroutine *c);

  void HandleRemoveSubsystem(const proto::RemoveSubsystemRequest &req,
                             proto::RemoveSubsystemResponse *response,
                             co::Coroutine *c);

  void HandleStartSubsystem(const proto::StartSubsystemRequest &req,
                            proto::StartSubsystemResponse *response,
                            co::Coroutine *c);

  void HandleStopSubsystem(const proto::StopSubsystemRequest &req,
                           proto::StopSubsystemResponse *response,
                           co::Coroutine *c);
  void HandleGetSubsystems(const proto::GetSubsystemsRequest &req,
                           proto::GetSubsystemsResponse *response,
                           co::Coroutine *c);
  void HandleGetAlarms(const proto::GetAlarmsRequest &req,
                       proto::GetAlarmsResponse *response, co::Coroutine *c);

  Capcom &capcom_;
  uint32_t id_;
};
} // namespace stagezero::capcom
