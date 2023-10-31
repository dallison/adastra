#include "stagezero/capcom/capcom.h"

namespace stagezero::capcom {

Capcom::Capcom(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
               int notify_fd, toolbelt::InetAddress stagezero)
    : co_scheduler_(scheduler), addr_(std::move(addr)), notify_fd_(notify_fd),
      stagezero_(std::move(stagezero)) {}

Capcom::~Capcom() {
  // Clear this before other data members get destroyed.
  client_handlers_.clear();
}

void Capcom::Stop() { co_scheduler_.Stop(); }

void Capcom::CloseHandler(std::shared_ptr<ClientHandler> handler) {
  for (auto it = client_handlers_.begin(); it != client_handlers_.end(); it++) {
    if (*it == handler) {
      client_handlers_.erase(it);
      break;
    }
  }
}

absl::Status
Capcom::HandleIncomingConnection(toolbelt::TCPSocket &listen_socket,
                                 co::Coroutine *c) {
  absl::StatusOr<toolbelt::TCPSocket> s = listen_socket.Accept(c);
  if (!s.ok()) {
    return s.status();
  }

  std::cout << "client handler address: " << s->BoundAddress().ToString()
            << std::endl;
  if (absl::Status status = s->SetCloseOnExec(); !status.ok()) {
    return status;
  }

  std::shared_ptr<ClientHandler> handler =
      std::make_shared<ClientHandler>(*this, std::move(*s));
  client_handlers_.push_back(handler);

  coroutines_.insert(std::make_unique<co::Coroutine>(
      co_scheduler_,
      [this, handler](co::Coroutine *c) {
        handler->Run(c);
        logger_.Log(toolbelt::LogLevel::kInfo, "client %s closed",
                    handler->GetClientName().c_str());
        CloseHandler(handler);
      },
      "Client handler"));

  return absl::OkStatus();
}

// This coroutine listens for incoming client connections on the given
// socket and spawns a handler coroutine to handle the communication with
// the client.
void Capcom::ListenerCoroutine(toolbelt::TCPSocket &listen_socket,
                               co::Coroutine *c) {
  for (;;) {
    absl::Status status = HandleIncomingConnection(listen_socket, c);
    if (!status.ok()) {
      logger_.Log(toolbelt::LogLevel::kError,
                  "Unable to make incoming connection: %s",
                  status.ToString().c_str());
    }
  }
}

absl::Status Capcom::Run() {
  toolbelt::TCPSocket listen_socket;

  if (absl::Status status = listen_socket.SetReuseAddr(); !status.ok()) {
    return status;
  }

  if (absl::Status status = listen_socket.SetReusePort(); !status.ok()) {
    return status;
  }

  if (absl::Status status = listen_socket.Bind(addr_, true); !status.ok()) {
    return status;
  }

  // Notify listener that we are ready.
  if (notify_fd_.Valid()) {
    int64_t val = kReady;
    (void)::write(notify_fd_.Fd(), &val, 8);
  }

  // Register a callback to be called when a coroutine completes.  The
  // server keeps track of all coroutines created.
  // This deletes them when they are done.
  co_scheduler_.SetCompletionCallback(
      [this](co::Coroutine *c) { coroutines_.erase(c); });

  // Start the listener coroutine.
  coroutines_.insert(
      std::make_unique<co::Coroutine>(co_scheduler_,
                                      [this, &listen_socket](co::Coroutine *c) {
                                        ListenerCoroutine(listen_socket, c);
                                      },
                                      "Listener Socket"));

  // Run the coroutine main loop.
  co_scheduler_.Run();

  // Notify that we are stopped.
  if (notify_fd_.Valid()) {
    int64_t val = kStopped;
    (void)::write(notify_fd_.Fd(), &val, 8);
  }

  return absl::OkStatus();
}
void Capcom::SendSubsystemStatusEvent(std::shared_ptr<Subsystem> subsystem) {
  for (auto &handler : client_handlers_) {
    if (absl::Status status = handler->SendSubsystemStatusEvent(subsystem);
        !status.ok()) {
      logger_.Log(toolbelt::LogLevel::kError,
                  "Failed to send event to client %s: %s",
                  handler->GetClientName().c_str(), status.ToString().c_str());
    }
  }
}

} // namespace stagezero::capcom