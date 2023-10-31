#pragma once

#include "stagezero/capcom/subsystem.h"
#include "stagezero/proto/capcom.pb.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "toolbelt/triggerfd.h"

#include "absl/container/flat_hash_map.h"
#include <list>

#include "coroutine.h"

namespace stagezero::capcom {

class Capcom;
class ClientHandler;

void EventSenderCoroutine(std::shared_ptr<ClientHandler> client,
                          co::Coroutine *c);

class ClientHandler : public std::enable_shared_from_this<ClientHandler> {
public:
  ClientHandler(Capcom &capcom, toolbelt::TCPSocket socket)
      : capcom_(capcom), command_socket_(std::move(socket)) {}
  ~ClientHandler();

  void Run(co::Coroutine *c);

  absl::Status SendSubsystemStatusEvent(std::shared_ptr<Subsystem> subsystem);

  const std::string &GetClientName() const { return client_name_; }

  co::CoroutineScheduler &GetScheduler() const;

  toolbelt::Logger &GetLogger() const;

  char *GetEventBuffer() { return event_buffer_; }

  toolbelt::TCPSocket &GetEventSocket() { return event_socket_; }
  void SetEventSocket(toolbelt::TCPSocket socket) {
    event_socket_ = std::move(socket);
  }

  void AddCoroutine(std::unique_ptr<co::Coroutine> c);

private:
  friend void EventSenderCoroutine(std::shared_ptr<ClientHandler> client,
                                   co::Coroutine *c);

  static constexpr size_t kMaxMessageSize = 4096;

  absl::Status HandleMessage(const stagezero::capcom::proto::Request &req,
                             stagezero::capcom::proto::Response &resp,
                             co::Coroutine *c);

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

  absl::Status QueueEvent(std::unique_ptr<stagezero::capcom::proto::Event> event);

  Capcom &capcom_;
  toolbelt::TCPSocket command_socket_;
  char command_buffer_[kMaxMessageSize];
  std::string client_name_ = "unknown";

  char event_buffer_[kMaxMessageSize];
  toolbelt::TCPSocket event_socket_;
  std::unique_ptr<co::Coroutine> event_channel_acceptor_;

  std::list<std::unique_ptr<capcom::proto::Event>> events_;
  toolbelt::TriggerFd event_trigger_;
};
} // namespace stagezero::capcom
