#include "flight/flight_director.h"

namespace stagezero::flight {

FlightDirector::FlightDirector(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
toolbelt::InetAddress capcom_addr,
               int notify_fd)
    : co_scheduler_(scheduler), addr_(std::move(addr)), capcom_addr_(capcom_addr),
    notify_fd_(notify_fd) {
}

FlightDirector::~FlightDirector() {
  // Clear this before other data members get destroyed.
  client_handlers_.clear();
}

void FlightDirector::Stop() { co_scheduler_.Stop(); }

void FlightDirector::CloseHandler(std::shared_ptr<ClientHandler> handler) {
  for (auto it = client_handlers_.begin(); it != client_handlers_.end(); it++) {
    if (*it == handler) {
      client_handlers_.erase(it);
      break;
    }
  }
}

absl::Status
FlightDirector::HandleIncomingConnection(toolbelt::TCPSocket &listen_socket,
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
void FlightDirector::ListenerCoroutine(toolbelt::TCPSocket &listen_socket,
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

absl::Status FlightDirector::Run() {
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

  // Connect to capcom.
  if (absl::Status status = capcom_client_.Init(capcom_addr_, "FlightDirector"); !status.ok()) {
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

}

