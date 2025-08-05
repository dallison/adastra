// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

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

namespace adastra::capcom {

class Capcom;

class ClientHandler
    : public common::TCPClientHandler<proto::Request, proto::Response,
                                      adastra::proto::Event> {
public:
  ClientHandler(Capcom &capcom, toolbelt::TCPSocket socket, uint32_t id);
  ~ClientHandler();

  absl::Status SendSubsystemStatusEvent(Subsystem *subsystem);
  absl::Status SendAlarm(const Alarm &alarm);
  absl::Status SendParameterUpdateEvent(const std::string &name,
                                        const parameters::Value &value);

  absl::Status SendParameterDeleteEvent(const std::string &name);
  absl::Status SendTelemetryEvent(const adastra::proto::TelemetryEvent &event);
  absl::Status SendOutputEvent(std::shared_ptr<adastra::proto::Event> event);

  co::CoroutineScheduler &GetScheduler() const override;

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) override;

  absl::Status SendOutputEvent(const std::string &process, int fd,
                               const char *data, size_t size);

  absl::Status SendLogEvent(std::shared_ptr<adastra::proto::LogMessage> msg);

  void Shutdown() override;

private:
  std::shared_ptr<ClientHandler> shared_from_this() {
    return std::static_pointer_cast<ClientHandler>(
        TCPClientHandler<proto::Request, proto::Response,
                         adastra::proto::Event>::shared_from_this());
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
  void HandleListComputes(const proto::ListComputesRequest &req,
                          proto::ListComputesResponse *response,
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

  void HandleRestartSubsystem(const proto::RestartSubsystemRequest &req,
                              proto::RestartSubsystemResponse *response,
                              co::Coroutine *c);
  void HandleRestartProcesses(const proto::RestartProcessesRequest &req,
                              proto::RestartProcessesResponse *response,
                              co::Coroutine *c);

  void HandleGetSubsystems(const proto::GetSubsystemsRequest &req,
                           proto::GetSubsystemsResponse *response,
                           co::Coroutine *c);
  void HandleGetAlarms(const proto::GetAlarmsRequest &req,
                       proto::GetAlarmsResponse *response, co::Coroutine *c);

  void HandleAbort(const proto::AbortRequest &req,
                   proto::AbortResponse *response, co::Coroutine *c);

  void HandleAddGlobalVariable(const proto::AddGlobalVariableRequest &req,
                               proto::AddGlobalVariableResponse *response,
                               co::Coroutine *c);

  void HandleInput(const proto::InputRequest &req,
                   proto::InputResponse *response, co::Coroutine *c);

  void HandleCloseFd(const proto::CloseFdRequest &req,
                     proto::CloseFdResponse *response, co::Coroutine *c);

  void HandleFreezeCgroup(const proto::FreezeCgroupRequest &req,
                          proto::FreezeCgroupResponse *response,
                          co::Coroutine *c);
  void HandleThawCgroup(const proto::ThawCgroupRequest &req,
                        proto::ThawCgroupResponse *response, co::Coroutine *c);
  void HandleKillCgroup(const proto::KillCgroupRequest &req,
                        proto::KillCgroupResponse *response, co::Coroutine *c);
  void HandleSetParameter(const proto::SetParameterRequest &req,
                          proto::SetParameterResponse *response,
                          co::Coroutine *c);
  void HandleDeleteParameters(const proto::DeleteParametersRequest &req,
                              proto::DeleteParametersResponse *response,
                              co::Coroutine *c);
  void HandleUploadParameters(const proto::UploadParametersRequest &req,
                              proto::UploadParametersResponse *response,
                              co::Coroutine *c);
  void HandleSendTelemetryCommand(const proto::SendTelemetryCommandRequest &req,
                                  proto::SendTelemetryCommandResponse *response,
                                  co::Coroutine *c);
  void HandleGetParameters(const proto::GetParametersRequest &req,
                           proto::GetParametersResponse *response,
                           co::Coroutine *c);
  void HandleAddCgroup(const proto::AddCgroupRequest &req,
                       proto::AddCgroupResponse *response, co::Coroutine *c);
  void HandleRemoveCgroups(const proto::RemoveCgroupsRequest &req,
                           proto::RemoveCgroupsResponse *response,
                           co::Coroutine *c);
  void HandleGetCgroups(const proto::GetCgroupsRequest &req,
                        proto::GetCgroupsResponse *response, co::Coroutine *c);
  Capcom &capcom_;
  uint32_t id_;
};
} // namespace adastra::capcom
