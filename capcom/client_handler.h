#pragma once

#include "capcom/subsystem.h"
#include "proto/capcom.pb.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "toolbelt/triggerfd.h"
#include "common/tcp_client_handler.h"

#include "absl/container/flat_hash_map.h"
#include <list>

#include "coroutine.h"

namespace stagezero::capcom {

class Capcom;

class ClientHandler : public common::TCPClientHandler<proto::Request, proto::Response,
                                      proto::Event>  {
public:
  ClientHandler(Capcom &capcom, toolbelt::TCPSocket socket)
      : TCPClientHandler(std::move(socket)), capcom_(capcom) {}
  ~ClientHandler();

  absl::Status SendSubsystemStatusEvent(std::shared_ptr<Subsystem> subsystem);

  co::CoroutineScheduler &GetScheduler() const override;

  toolbelt::Logger &GetLogger() const override;

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) override;

private:
  absl::Status HandleMessage(const stagezero::capcom::proto::Request &req,
                             stagezero::capcom::proto::Response &resp,
                             co::Coroutine *c) override;

  void HandleInit(const stagezero::capcom::proto::InitRequest &req,
                  stagezero::capcom::proto::InitResponse *response,
                  co::Coroutine *c);

  void
  HandleAddSubsystem(const stagezero::capcom::proto::AddSubsystemRequest &req,
                     stagezero::capcom::proto::AddSubsystemResponse *response,
                     co::Coroutine *c);

  void HandleRemoveSubsystem(
      const stagezero::capcom::proto::RemoveSubsystemRequest &req,
      stagezero::capcom::proto::RemoveSubsystemResponse *response,
      co::Coroutine *c);

  void HandleStartSubsystem(
      const stagezero::capcom::proto::StartSubsystemRequest &req,
      stagezero::capcom::proto::StartSubsystemResponse *response,
      co::Coroutine *c);

  void
  HandleStopSubsystem(const stagezero::capcom::proto::StopSubsystemRequest &req,
                      stagezero::capcom::proto::StopSubsystemResponse *response,
                      co::Coroutine *c);

  Capcom &capcom_;
};
} // namespace stagezero::capcom
