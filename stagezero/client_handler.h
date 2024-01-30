// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "common/tcp_client_handler.h"
#include "proto/config.pb.h"
#include "proto/control.pb.h"
#include "proto/log.pb.h"
#include "stagezero/process.h"
#include "stagezero/symbols.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "toolbelt/triggerfd.h"

#include <list>
#include "absl/container/flat_hash_map.h"

#include "coroutine.h"

namespace adastra::stagezero {

class StageZero;

class ClientHandler
    : public common::TCPClientHandler<control::Request, control::Response,
                                      control::Event> {
 public:
  ClientHandler(StageZero &stagezero, toolbelt::TCPSocket socket)
      : TCPClientHandler(std::move(socket)), stagezero_(stagezero) {}
  ~ClientHandler();

  absl::Status SendProcessStartEvent(const std::string &process_id);
  absl::Status SendProcessStopEvent(const std::string &process_id, bool exited,
                                    int exit_status, int term_signal);
  absl::Status SendOutputEvent(const std::string &process_id, int fd,
                               const char *data, size_t len);

  toolbelt::Logger &GetLogger() const override;

  co::CoroutineScheduler &GetScheduler() const override;

  SymbolTable *GetGlobalSymbols() const;

  std::shared_ptr<Zygote> FindZygote(const std::string &name) const;

  absl::Status RemoveProcess(Process *proc);

  // Try to remove the process.  If it doesn't exist we're good.
  void TryRemoveProcess(std::shared_ptr<Process> proc);

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) override;

  void StopAllCoroutines();
  
  const std::string &GetCompute() const;

  StageZero& GetStageZero() const { return stagezero_; }
  
 private:
  std::shared_ptr<ClientHandler> shared_from_this() {
    return std::static_pointer_cast<ClientHandler>(
        TCPClientHandler<control::Request, control::Response,
                         control::Event>::shared_from_this());
  }

  absl::Status HandleMessage(const control::Request &req,
                             control::Response &resp,
                             co::Coroutine *c) override;

  void HandleInit(const control::InitRequest &req,
                  control::InitResponse *response, co::Coroutine *c);

  void HandleLaunchStaticProcess(
      const control::LaunchStaticProcessRequest &&req,
      control::LaunchResponse *response, co::Coroutine *c);

  void HandleLaunchZygote(const control::LaunchStaticProcessRequest &&req,
                          control::LaunchResponse *response, co::Coroutine *c);

  void HandleLaunchVirtualProcess(
      const control::LaunchVirtualProcessRequest &&req,
      control::LaunchResponse *response, co::Coroutine *c);

  void HandleStopProcess(const control::StopProcessRequest &req,
                         control::StopProcessResponse *response,
                         co::Coroutine *c);

  void HandleInputData(const control::InputDataRequest &req,
                       control::InputDataResponse *response, co::Coroutine *c);

  void HandleSetGlobalVariable(const control::SetGlobalVariableRequest &req,
                               control::SetGlobalVariableResponse *response,
                               co::Coroutine *c);

  void HandleGetGlobalVariable(const control::GetGlobalVariableRequest &req,
                               control::GetGlobalVariableResponse *response,
                               co::Coroutine *c);

  void HandleCloseProcessFileDescriptor(
      const control::CloseProcessFileDescriptorRequest &req,
      control::CloseProcessFileDescriptorResponse *response, co::Coroutine *c);

  void HandleAbort(const control::AbortRequest &req,
                   control::AbortResponse *response, co::Coroutine *c);

void HandleAddCgroup(const control::AddCgroupRequest &req,
                   control::AddCgroupResponse *response, co::Coroutine *c);

  void HandleRemoveCgroup(const control::RemoveCgroupRequest &req,
                   control::RemoveCgroupResponse *response, co::Coroutine *c);   
                                 
  void AddProcess(const std::string &id, std::shared_ptr<Process> proc) {
    processes_.emplace(std::make_pair(id, std::move(proc)));
  }

  void KillAllProcesses();

  StageZero &stagezero_;

  // Keep track of the processs we spawned here.
  absl::flat_hash_map<std::string, std::shared_ptr<Process>> processes_;
};
}  // namespace adastra::stagezero
