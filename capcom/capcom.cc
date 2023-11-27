// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "capcom/capcom.h"

namespace stagezero::capcom {

Capcom::Capcom(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
               const std::string &log_file_name, int notify_fd)
    : co_scheduler_(scheduler), addr_(std::move(addr)), notify_fd_(notify_fd) {
  // TODO: how to get the port for stagezero.
  local_compute_ = {.name = "<localhost>",
                    .addr = toolbelt::InetAddress("localhost", 6522)};

  // Create the log message pipe.
  int pipes[2];
  int e = ::pipe(pipes);
  if (e == -1) {
    std::cerr << "Failed to create logging pipe: " << strerror(errno)
              << std::endl;
    abort();
  }
  incoming_log_message_.SetFd(pipes[0]);
  log_message_.SetFd(pipes[1]);

  if (!log_file_name.empty()) {
    log_file_.SetFd(
        open(log_file_name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777));
    if (!log_file_.Valid()) {
      std::cerr << "Failed to open log file: " << log_file_name << ": "
                << strerror(errno) << std::endl;
      abort();
    }
  }
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

absl::Status Capcom::HandleIncomingConnection(
    toolbelt::TCPSocket &listen_socket, co::Coroutine *c) {
  absl::StatusOr<toolbelt::TCPSocket> s = listen_socket.Accept(c);
  if (!s.ok()) {
    return s.status();
  }

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
        logger_.Log(toolbelt::LogLevel::kDebug, "client %s closed",
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

void Capcom::Log(const stagezero::control::LogMessage &msg) {
  uint64_t size = msg.ByteSizeLong();
  // Write length prefix.
  ssize_t n = ::write(log_message_.Fd(), &size, sizeof(size));
  if (n <= 0) {
    logger_.Log(toolbelt::LogLevel::kError,
                "Failed to write to logger pipe: %s", strerror(errno));
    return;
  }
  if (!msg.SerializeToFileDescriptor(log_message_.Fd())) {
    logger_.Log(toolbelt::LogLevel::kError,
                "Failed to serialize to logger pipe: %s", strerror(errno));
  }
}

void Capcom::LoggerCoroutine(co::Coroutine *c) {
  for (;;) {
    c->Wait(incoming_log_message_.Fd());
    uint64_t size;
    ssize_t n = ::read(incoming_log_message_.Fd(), &size, sizeof(uint64_t));
    if (n <= 0) {
      std::cerr << "Failed to read log message: " << strerror(errno)
                << std::endl;
      return;
    }
    auto msg = std::make_unique<control::LogMessage>();
    std::vector<char> buffer(size);
    n = ::read(incoming_log_message_.Fd(), buffer.data(), buffer.size());
    if (n <= 0) {
      std::cerr << "Failed to parse log message: " << strerror(errno)
                << std::endl;
      return;
    }
    if (!msg->ParseFromArray(buffer.data(), buffer.size())) {
      std::cerr << "Failed to deserialize log message: " << strerror(errno)
                << std::endl;
      return;
    }

    // Add log message to the log message buffer in timestamp order.
    log_buffer_.insert(std::make_pair(msg->timestamp(), std::move(msg)));
  }
}

void Capcom::LoggerFlushCoroutine(co::Coroutine *c) {
  for (;;) {
    c->Millisleep(500);  // Flush the log buffer every 500ms.
    for (auto & [ timestamp, msg ] : log_buffer_) {
      toolbelt::LogLevel level;
      switch (msg->level()) {
        case control::LogMessage::VERBOSE:
          level = toolbelt::LogLevel::kVerboseDebug;
          break;
        case control::LogMessage::DBG:
          level = toolbelt::LogLevel::kDebug;
          break;
        case control::LogMessage::INFO:
          level = toolbelt::LogLevel::kInfo;
          break;
        case control::LogMessage::WARNING:
          level = toolbelt::LogLevel::kWarning;
          break;
        case control::LogMessage::ERR:
          level = toolbelt::LogLevel::kError;
          break;
        default:
          continue;
      }
      logger_.Log(level, timestamp, msg->process_name(), msg->text());

      if (log_file_.Valid()) {
        // Serialize into the log file.
        // TODO: I don't think the multiple serializations will affect anything
        // because this is a low volume channel and there is a much longer path
        // from the process running to this point.
        uint64_t size = msg->ByteSizeLong();
        ssize_t n = ::write(log_file_.Fd(), &size, sizeof(size));
        if (n <= 0) {
          logger_.Log(toolbelt::LogLevel::kError,
                      "Failed to write to log file: %s", strerror(errno));
        } else {
          if (!msg->SerializeToFileDescriptor(log_file_.Fd())) {
            logger_.Log(toolbelt::LogLevel::kError,
                        "Failed to serialize to log file: %s", strerror(errno));
          }
        }
      }
    }
    log_buffer_.clear();
  }
}

absl::Status Capcom::Run() {
  logger_.Log(toolbelt::LogLevel::kInfo, "Capcom running on address %s", addr_.ToString().c_str());

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

  // Start the logger coroutines.
  coroutines_.insert(std::make_unique<co::Coroutine>(
      co_scheduler_, [this](co::Coroutine *c) { LoggerCoroutine(c); },
      "Logger"));

  coroutines_.insert(std::make_unique<co::Coroutine>(
      co_scheduler_, [this](co::Coroutine *c) { LoggerFlushCoroutine(c); },
      "Log Flusher"));

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

std::vector<Alarm> Capcom::GetAlarms() const {
  std::vector<Alarm> result;
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
  for (;;) {
    bool all_offline = true;
    for (auto & [ name, subsys ] : subsystems_) {
      if (!subsys->IsOffline()) {
        all_offline = false;
        break;
      }
    }
    if (all_offline) {
      break;
    }
    c->Millisleep(20);
  }

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

absl::Status Capcom::AddGlobalVariable(const Variable &var, co::Coroutine *c) {
  std::vector<Compute *> computes;
  absl::Status result = absl::OkStatus();

  for (auto & [ name, compute ] : computes_) {
    computes.push_back(&compute);
  }

  // If we have no computes (like in testing), add the local compute.
  if (computes.empty()) {
    computes.push_back(&local_compute_);
  }

  for (auto &compute : computes) {
    stagezero::Client client;
    if (absl::Status status =
            client.Init(compute->addr, "<set global variable>");
        !status.ok()) {
      result = absl::InternalError(
          absl::StrFormat("Failed to connect compute for abort%s: %s",
                          compute->name, status.ToString()));
      continue;
    }
    absl::Status status =
        client.SetGlobalVariable(var.name, var.value, var.exported, c);
    if (!status.ok()) {
      result = absl::InternalError(
          absl::StrFormat("Failed to set global variable %s on compute %s: %s",
                          var.name, compute->name, status.ToString()));
    }
  }
  return result;
}

}  // namespace stagezero::capcom