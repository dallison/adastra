// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "capcom/capcom.h"

namespace stagezero::capcom {

Capcom::Capcom(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
               int notify_fd)
    : co_scheduler_(scheduler), addr_(std::move(addr)), notify_fd_(notify_fd) {
  // TODO: how to get the port for stagezero.
  local_compute_ = {.name = "<localhost>",
                    .addr = toolbelt::InetAddress("localhost", 6522)};
}

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

  uint32_t client_id = client_ids_.Allocate();
  std::shared_ptr<ClientHandler> handler =
      std::make_shared<ClientHandler>(*this, std::move(*s), client_id);
  client_handlers_.push_back(handler);

  coroutines_.insert(std::make_unique<co::Coroutine>(
      co_scheduler_,
      [this, handler, client_id](co::Coroutine *c) {
        handler->Run(c);
        logger_.Log(toolbelt::LogLevel::kInfo, "client %s closed",
                    handler->GetClientName().c_str());
        client_ids_.Clear(client_id);
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
  std::cerr << "Capcom running on address " << addr_.ToString() << std::endl;

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
void Capcom::SendSubsystemStatusEvent(Subsystem *subsystem) {
  for (auto &handler : client_handlers_) {
    if (absl::Status status = handler->SendSubsystemStatusEvent(subsystem);
        !status.ok()) {
      logger_.Log(toolbelt::LogLevel::kError,
                  "Failed to send event to client %s: %s",
                  handler->GetClientName().c_str(), status.ToString().c_str());
    }
  }
}

void Capcom::SendAlarm(const Alarm &alarm) {
  for (auto &handler : client_handlers_) {
    if (absl::Status status = handler->SendAlarm(alarm); !status.ok()) {
      logger_.Log(toolbelt::LogLevel::kError,
                  "Failed to send alarm to client %s: %s",
                  handler->GetClientName().c_str(), status.ToString().c_str());
    }
  }
}

std::vector<Subsystem *> Capcom::GetSubsystems() const {
  std::vector<Subsystem *> result;
  for (auto &s : subsystems_) {
    result.push_back(s.second.get());
  }
  return result;
}

std::vector<Alarm *> Capcom::GetAlarms() const {
  std::vector<Alarm *> result;
  for (auto &s : subsystems_) {
    s.second->CollectAlarms(result);
  }
  return result;
}

absl::Status Capcom::Abort(const std::string &reason, co::Coroutine *c) {
  // First take the subsystems offline with an abort.  This will not
  // kill any running processes.
  absl::Status result = absl::OkStatus();

  for (auto & [ name, subsys ] : subsystems_) {
    Message msg = {.code = Message::Code::kAbort};
    if (absl::Status status = subsys->SendMessage(msg); !status.ok()) {
      result = status;
    }
  }

  // Make sure all the subsystems are admin offline, oper offline.  If we
  // go ahead and kill the processes without waiting, the subsystems will
  // get notified that their process has died and will attempt to restart it.
  std::cerr << "waiting for subsystems to go offline\n";
  for (;;) {
    bool all_offline = true;
    for (auto & [ name, subsys ] : subsystems_) {
      std::cerr << "checking subsystem " << subsys->Name() << " "
                << subsys->IsOffline() << std::endl;
      if (!subsys->IsOffline()) {
        std::cerr << subsys->Name() << " is not offline\n";
        all_offline = false;
        break;
      }
    }
    if (all_offline) {
      break;
    }
    c->Millisleep(20);
  }

  std::cerr << " all subsystems offline\n";
  // Now tell all computes (the stagezero running on them) to kill
  // all the processes.
  std::vector<Compute *> computes;

  for (auto & [ name, compute ] : computes_) {
    computes.push_back(&compute);
  }

  // If we have no computes (like in testing), add the local compute.
  if (computes.empty()) {
    computes.push_back(&local_compute_);
  }

  for (auto &compute : computes) {
    stagezero::Client client;
    std::cerr << "sending abort to " << compute->name << std::endl;
    if (absl::Status status = client.Init(compute->addr, "<capcom abort>");
        !status.ok()) {
      result = absl::InternalError(
          absl::StrFormat("Failed to connect compute for abort%s: %s",
                          compute->name, status.ToString()));
      continue;
    }
    absl::Status status = client.Abort(reason);
    if (!status.ok()) {
      result = absl::InternalError(absl::StrFormat(
          "Failed to abort compute %s: %s", compute->name, status.ToString()));
    }
  }
  return result;
}

} // namespace stagezero::capcom