#pragma once

#include "stagezero/proto/config.pb.h"
#include "stagezero/proto/control.pb.h"
#include "stagezero/process.h"
#include "stagezero/symbols.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "toolbelt/triggerfd.h"
#include <list>
#include "absl/container/flat_hash_map.h"

#include "coroutine.h"

namespace stagezero {

class StageZero;

class ClientHandler : public std::enable_shared_from_this<ClientHandler>{
public:
  ClientHandler(StageZero &stagezero, toolbelt::TCPSocket socket)
      : stagezero_(stagezero), command_socket_(std::move(socket)) {}
  ~ClientHandler();

  void Run(co::Coroutine *c);

  absl::Status SendProcessStartEvent(const std::string &process_id);
  absl::Status SendProcessStopEvent(const std::string &process_id, bool exited,
                                    int exit_status, int term_signal);
  absl::Status SendOutputEvent(const std::string &process_id, int fd,
                               const char *data, size_t len);

  const std::string &GetClientName() const { return client_name_; }

  toolbelt::Logger &GetLogger() const;

  co::CoroutineScheduler &GetScheduler() const;

  SymbolTable* GetGlobalSymbols() const;

  std::shared_ptr<Zygote> FindZygote(const std::string& name) const;

  absl::Status RemoveProcess(Process* proc);

  // Try to remove the process.  If it doesn't exist we're good.
  void TryRemoveProcess(std::shared_ptr<Process> proc);

  void AddCoroutine(std::unique_ptr<co::Coroutine> c);

  char *GetEventBuffer() { return event_buffer_; }

  toolbelt::TCPSocket& GetEventSocket() { return event_socket_; }
  void SetEventSocket(toolbelt::TCPSocket socket) {
    event_socket_ = std::move(socket);
  }

private:
  friend void EventSenderCoroutine(std::shared_ptr<ClientHandler> client, co::Coroutine *c);

  static constexpr size_t kMaxMessageSize = 4096;

  absl::Status HandleMessage(const stagezero::control::Request &req,
                             stagezero::control::Response &resp,
                             co::Coroutine *c);

  void HandleInit(const stagezero::control::InitRequest &req,
                  stagezero::control::InitResponse *response, co::Coroutine *c);

  void HandleLaunchStaticProcess(
      const stagezero::control::LaunchStaticProcessRequest &&req,
      stagezero::control::LaunchResponse *response, co::Coroutine *c);

  void HandleLaunchZygote(
      const stagezero::control::LaunchStaticProcessRequest &&req,
      stagezero::control::LaunchResponse *response, co::Coroutine *c);

  void HandleLaunchVirtualProcess(
      const stagezero::control::LaunchVirtualProcessRequest &&req,
      stagezero::control::LaunchResponse *response, co::Coroutine *c);

  void HandleStopProcess(const stagezero::control::StopProcessRequest &req,
                         stagezero::control::StopProcessResponse *response,
                         co::Coroutine *c);

  void HandleInputData(const stagezero::control::InputDataRequest &req,
                       stagezero::control::InputDataResponse *response,
                       co::Coroutine *c);

  void HandleSetGlobalVariable(const stagezero::control::SetGlobalVariableRequest &req,
                       stagezero::control::SetGlobalVariableResponse *response,
                       co::Coroutine *c);

     void HandleGetGlobalVariable(const stagezero::control::GetGlobalVariableRequest &req,
                       stagezero::control::GetGlobalVariableResponse *response,
                       co::Coroutine *c);      

  void HandleCloseProcessFileDescriptor(
      const stagezero::control::CloseProcessFileDescriptorRequest &req,
      stagezero::control::CloseProcessFileDescriptorResponse *response,
      co::Coroutine *c);

  absl::Status QueueEvent(std::unique_ptr<stagezero::control::Event> event);

  void AddProcess(const std::string &id, std::shared_ptr<Process> proc) {
    processes_.emplace(std::make_pair(id, std::move(proc)));
  }

  void KillAllProcesses();

  StageZero &stagezero_;
  toolbelt::TCPSocket command_socket_;
  char command_buffer_[kMaxMessageSize];
  std::string client_name_ = "unknown";

  char event_buffer_[kMaxMessageSize];
  toolbelt::TCPSocket event_socket_;
  std::unique_ptr<co::Coroutine> event_channel_acceptor_;

  // Keep track of the processs we spawned here, but we don't own the
  // process object.
  absl::flat_hash_map<std::string, std::shared_ptr<Process>> processes_;

  std::list<std::unique_ptr<control::Event>> events_;
  toolbelt::TriggerFd event_trigger_;
};
} // namespace stagezero
