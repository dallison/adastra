#include "stagezero/capcom/client_handler.h"
#include "absl/strings/str_format.h"
#include "stagezero/capcom/capcom.h"
#include "toolbelt/hexdump.h"

#include <iostream>

namespace stagezero::capcom {

ClientHandler::~ClientHandler() {}

void ClientHandler::Run(co::Coroutine *c) {
  // The data is placed 4 bytes into the buffer.  The first 4
  // bytes of the buffer are used by SendMessage and ReceiveMessage
  // for the length of the data.
  std::cerr << "client handler running" << std::endl;
  char *sendbuf = command_buffer_ + sizeof(int32_t);
  constexpr size_t kSendBufLen = sizeof(command_buffer_) - sizeof(int32_t);
  for (;;) {
    absl::StatusOr<ssize_t> n = command_socket_.ReceiveMessage(
        command_buffer_, sizeof(command_buffer_), c);
    if (!n.ok()) {
      printf("ReceiveMessage error %s\n", n.status().ToString().c_str());
      return;
    }
    std::cout << getpid() << " " << this << " capcom received\n";
    toolbelt::Hexdump(command_buffer_, *n);

    stagezero::capcom::proto::Request request;
    if (request.ParseFromArray(command_buffer_, *n)) {
      stagezero::capcom::proto::Response response;
      if (absl::Status s = HandleMessage(request, response, c); !s.ok()) {
        capcom_.logger_.Log(toolbelt::LogLevel::kError, "%s\n",
                            s.ToString().c_str());
        return;
      }

      std::cout << getpid() << " capcom sending " << response.DebugString()
                << std::endl;
      if (!response.SerializeToArray(sendbuf, kSendBufLen)) {
        capcom_.logger_.Log(toolbelt::LogLevel::kError,
                            "Failed to serialize response\n");
        return;
      }
      size_t msglen = response.ByteSizeLong();
      std::cout << getpid() << " CAPCOM SEND\n";
      toolbelt::Hexdump(sendbuf, msglen);
      absl::StatusOr<ssize_t> n =
          command_socket_.SendMessage(sendbuf, msglen, c);
      if (!n.ok()) {
        return;
      }
    } else {
      capcom_.logger_.Log(toolbelt::LogLevel::kError,
                          "Failed to parse message\n");
      return;
    }
  }
}

  absl::Status ClientHandler::SendSubsystemStatusEvent(std::shared_ptr<Subsystem> subsystem) {
  auto event = std::make_unique<capcom::proto::Event>();
  auto s = event->mutable_subsystem_status();
  subsystem->BuildStatusEvent(s);
  return QueueEvent(std::move(event));
  }

absl::Status
ClientHandler::HandleMessage(const stagezero::capcom::proto::Request &req,
                             stagezero::capcom::proto::Response &resp,
                             co::Coroutine *c) {
  std::cerr << "capcom handling message " << req.DebugString() << std::endl;
  switch (req.request_case()) {
  case stagezero::capcom::proto::Request::kInit:
    HandleInit(req.init(), resp.mutable_init(), c);
    break;
  case stagezero::capcom::proto::Request::kAddSubsystem:
    HandleAddSubsystem(req.add_subsystem(), resp.mutable_add_subsystem(), c);
    break;

  case stagezero::capcom::proto::Request::kRemoveSubsystem:
    HandleRemoveSubsystem(req.remove_subsystem(),
                          resp.mutable_remove_subsystem(), c);
    break;
  case stagezero::capcom::proto::Request::kStartSubsystem:
    HandleStartSubsystem(req.start_subsystem(), resp.mutable_start_subsystem(),
                         c);
    break;
  case stagezero::capcom::proto::Request::kStopSubsystem:
    HandleStopSubsystem(req.stop_subsystem(), resp.mutable_stop_subsystem(), c);
    break;

  case stagezero::capcom::proto::Request::REQUEST_NOT_SET:
    return absl::InternalError("Protocol error: unknown request");
  }
  return absl::OkStatus();
}

void ClientHandler::HandleInit(const stagezero::capcom::proto::InitRequest &req,
                               stagezero::capcom::proto::InitResponse *response,
                               co::Coroutine *c) {
  client_name_ = req.client_name();

   // Event channel is an ephemeral port.
  toolbelt::InetAddress event_channel_addr = command_socket_.BoundAddress();
  event_channel_addr.SetPort(0);

  std::cout << "binding event channel to " << event_channel_addr.ToString()
            << std::endl;
  // Open listen socket.
  toolbelt::TCPSocket listen_socket;
  if (absl::Status status = listen_socket.SetCloseOnExec(); !status.ok()) {
    response->set_error(status.ToString());
    return;
  }

  if (absl::Status status = listen_socket.Bind(event_channel_addr, true);
      !status.ok()) {
    response->set_error(status.ToString());
    return;
  }
  int event_port = listen_socket.BoundAddress().Port();

  response->set_event_port(event_port);

  if (absl::Status status = event_trigger_.Open(); !status.ok()) {
    response->set_error(status.ToString());
    return;
  }

  // Spawn a coroutine to accept the event channel connection.
  co::Coroutine* acceptor = new co::Coroutine(
   capcom_.co_scheduler_, [
        client = shared_from_this(), listen_socket = std::move(listen_socket)
      ](co::Coroutine * c2) mutable {
        std::cout << "accepting event channel " << std::endl;
        absl::StatusOr socket = listen_socket.Accept(c2);
        if (!socket.ok()) {
          client->GetLogger().Log(toolbelt::LogLevel::kError,
                                  "Failed to open event channel: %s",
                                  socket.status().ToString().c_str());
          return;
        }

        if (absl::Status status = socket->SetCloseOnExec(); !status.ok()) {
          client->GetLogger().Log(
              toolbelt::LogLevel::kError,
              "Failed to set close-on-exec on event channel: %s",
              socket.status().ToString().c_str());
          return;
        }
        client->event_socket_ = std::move(*socket);
        client->GetLogger().Log(toolbelt::LogLevel::kInfo,
                                "Event channel open");

        // Start the event sender coroutine.
        client->AddCoroutine(std::make_unique<co::Coroutine>(
            client->GetScheduler(),
            [client](co::Coroutine *c2) { EventSenderCoroutine(client, c2); }));
      });
      event_channel_acceptor_ = std::unique_ptr<co::Coroutine>(acceptor);
}

void ClientHandler::AddCoroutine(std::unique_ptr<co::Coroutine> c) {
  capcom_.AddCoroutine(std::move(c));
}

void ClientHandler::HandleAddSubsystem(
    const stagezero::capcom::proto::AddSubsystemRequest &req,
    stagezero::capcom::proto::AddSubsystemResponse *response,
    co::Coroutine *c) {
  std::cerr << "HandleAddSubsystem" << std::endl;
  auto subsystem = std::make_shared<Subsystem>(req.name(), capcom_);
  if (!capcom_.AddSubsystem(req.name(), subsystem)) {
    response->set_error(absl::StrFormat("Failed to add subsystem %s; already exists", req.name()));
    return;
  }

  // Add the processes to the subsystem.
  for (auto &proc : req.processes()) {
    switch (proc.proc_case()) {
    case stagezero::capcom::proto::Process::kStaticProcess:
      if (absl::Status status = subsystem->AddStaticProcess(
              proc.static_process(), proc.options());
          !status.ok()) {
        response->set_error(
            absl::StrFormat("Failed to add static process %s: %s",
                            proc.options().name(), status.ToString()));
        return;
      }
      break;
    case stagezero::capcom::proto::Process::kZygote:
      break;
    case stagezero::capcom::proto::Process::kVirtualProcess:
      break;
    case stagezero::capcom::proto::Process::PROC_NOT_SET:
      break;
    }
  }

  // Start the subsystem running.  This spawns a coroutine and returns without
  // bloocking.
  subsystem->Run();
}

void ClientHandler::HandleRemoveSubsystem(
    const stagezero::capcom::proto::RemoveSubsystemRequest &req,
    stagezero::capcom::proto::RemoveSubsystemResponse *response,
    co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  if (absl::Status status = subsystem->Remove(req.recursive()); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to remove subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
  }
}

void ClientHandler::HandleStartSubsystem(
    const stagezero::capcom::proto::StartSubsystemRequest &req,
    stagezero::capcom::proto::StartSubsystemResponse *response,
    co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  Message message = {.code = Message::kChangeAdmin, .state.admin = AdminState::kOnline};
  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to start subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
    return;
  }
}

void ClientHandler::HandleStopSubsystem(
    const stagezero::capcom::proto::StopSubsystemRequest &req,
    stagezero::capcom::proto::StopSubsystemResponse *response,
    co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  Message message = {.code = Message::kChangeAdmin, .state.admin = AdminState::kOffline};
  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to stop subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
    return;
  }
}

absl::Status
ClientHandler::QueueEvent(std::unique_ptr<stagezero::capcom::proto::Event> event) {
  if (!event_socket_.Connected()) {
    return absl::InternalError(
        "Unable to send event: event socket is not connected");
  }
  std::cerr << "queueing event" << std::endl;
  events_.push_back(std::move(event));
  event_trigger_.Trigger();
  return absl::OkStatus();
}

void EventSenderCoroutine(std::shared_ptr<ClientHandler> client,
                          co::Coroutine *c) {
  while (client->event_socket_.Connected()) {
    std::cerr << "Waiting for event to be queued" << std::endl;
    // Wait for an event to be queued.
    c->Wait(client->event_trigger_.GetPollFd().Fd(), POLLIN);
    client->event_trigger_.Clear();

    // Empty event queue by sending all events before going back
    // to waiting for the trigger.  This ensures that we never have
    // something in the event queue after the trigger has been
    // cleared.
    while (!client->events_.empty()) {
      std::cerr << "Got queued event" << std::endl;

      // Remove event from the queue and send it.
      std::unique_ptr<capcom::proto::Event> event =
          std::move(client->events_.front());
      client->events_.pop_front();
      std::cerr << event->DebugString() << std::endl;

      char *sendbuf = client->event_buffer_ + sizeof(int32_t);
      constexpr size_t kSendBufLen =
          sizeof(client->event_buffer_) - sizeof(int32_t);
      if (!event->SerializeToArray(sendbuf, kSendBufLen)) {
        client->GetLogger().Log(toolbelt::LogLevel::kError,
                                "Failed to serialize event");
      } else {
        size_t msglen = event->ByteSizeLong();
        absl::StatusOr<ssize_t> n =
            client->event_socket_.SendMessage(sendbuf, msglen, c);
        if (!n.ok()) {
          client->GetLogger().Log(toolbelt::LogLevel::kError,
                                  "Failed to serialize event",
                                  n.status().ToString().c_str());
        }
      }
    }
  }
}
} // namespace stagezero::capcom
