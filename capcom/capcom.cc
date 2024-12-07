// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "capcom/capcom.h"
#include "toolbelt/clock.h"

namespace adastra::capcom {

Capcom::Capcom(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
               bool log_to_output, int local_stagezero_port,
               std::string log_file_name, std::string log_level, bool test_mode,
               int notify_fd)
    : co_scheduler_(scheduler), addr_(std::move(addr)),
      log_to_output_(log_to_output), test_mode_(test_mode),
      notify_fd_(notify_fd), logger_("capcom", log_to_output) {
  logger_.SetLogLevel(log_level);
  Compute lc = {.name = "<localhost>",
                .addr =
                    toolbelt::InetAddress("localhost", local_stagezero_port)};
  local_compute_ = std::make_shared<Compute>(lc);

  // Create the log message pipe.
  absl::StatusOr<toolbelt::Pipe> p = toolbelt::Pipe::Create();
  if (!p.ok()) {
    std::cerr << "Failed to create logging pipe: " << strerror(errno)
              << std::endl;
    abort();
  }
  log_pipe_ = std::move(*p);

  // Make a log file name with the current local time and data.
  if (log_file_name.empty()) {
    char timebuf[64];
    struct tm tm;
    struct timespec now_ts;
    clock_gettime(CLOCK_REALTIME, &now_ts);

    size_t n = strftime(timebuf, sizeof(timebuf), "%FT%T",
                        localtime_r(&now_ts.tv_sec, &tm));
    if (n == 0) {
      log_file_name = "/tmp/capcom.pb";
    } else {
      log_file_name = absl::StrFormat("/tmp/capcom-%s.pb", timebuf);
    }
  }
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

absl::Status
Capcom::HandleIncomingConnection(toolbelt::TCPSocket &listen_socket,
                                 co::Coroutine *c) {
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
        handler->Shutdown();
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

void Capcom::Log(const adastra::proto::LogMessage &msg) {
  uint64_t size = msg.ByteSizeLong();
  // Write length prefix.
  ssize_t n = ::write(log_pipe_.WriteFd().Fd(), &size, sizeof(size));
  if (n <= 0) {
    logger_.Log(toolbelt::LogLevel::kError,
                "Failed to write to logger pipe: %s", strerror(errno));
    return;
  }
  if (!msg.SerializeToFileDescriptor(log_pipe_.WriteFd().Fd())) {
    logger_.Log(toolbelt::LogLevel::kError,
                "Failed to serialize to logger pipe: %s", strerror(errno));
  }
}

absl::Status Capcom::ConnectUmbilical(const std::string &compute,
                                      co::Coroutine *c) {
  Umbilical *umbilical = FindUmbilical(compute);
  if (umbilical == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No umbilical found for compute %s", compute));
  }

  umbilical->Precondition();
  if (absl::Status status = umbilical->Connect(kNoEvents, c); !status.ok()) {
    return status;
  }
 
  // Register cgroups
  if (absl::Status add_status =
          RegisterComputeCgroups(umbilical->GetClient(), umbilical->GetCompute(), c);
      !add_status.ok()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to add cgroup to compute %s: %s",
        umbilical->GetCompute()->name.c_str(), add_status.ToString().c_str()));
  }

  // Upload all the parameters to stagezero.
  std::vector<parameters::Parameter> parameters =
      parameters_.GetAllParameters();
  if (parameters.empty()) {
    // No parameters, but StageZero might have some from the last time we
    // connected, delete them.
    if (absl::Status s = umbilical->GetClient()->DeleteParameters({}, c); !s.ok()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to delete parameters from compute %s: %s",
          umbilical->GetCompute()->name.c_str(), s.ToString().c_str()));
    }
  }
  // Send the parameters to the client.
  if (absl::Status status = umbilical->GetClient()->UploadParameters(parameters, c);
      !status.ok()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to upload parameters to compute %s: %s",
        umbilical->GetCompute()->name.c_str(), status.ToString().c_str()));
  }

  // Add all global symbols to stagezero.
  for (auto & [ name, sym ] : global_symbols_.GetSymbols()) {
    if (absl::Status status = umbilical->GetClient()->SetGlobalVariable(
            sym->Name(), sym->Value(), sym->Exported(), c);
        !status.ok()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to set global variable %s on %s: %s", sym->Name().c_str(),
          umbilical->GetCompute()->name.c_str(), status.ToString().c_str()));
    }
  }
  return absl::OkStatus();
}

void Capcom::LoggerCoroutine(co::Coroutine *c) {
  for (;;) {
    c->Wait(log_pipe_.ReadFd().Fd());
    uint64_t size;
    ssize_t n = ::read(log_pipe_.ReadFd().Fd(), &size, sizeof(uint64_t));
    if (n <= 0) {
      std::cerr << "Failed to read log message: " << strerror(errno)
                << std::endl;
      return;
    }
    auto msg = std::make_shared<adastra::proto::LogMessage>();
    std::vector<char> buffer(size);
    n = ::read(log_pipe_.ReadFd().Fd(), buffer.data(), buffer.size());
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

void Capcom::FlushLogs() {
  // If there is no client wants log events, log it to the local logger.
  bool client_wants_events = false;
  for (auto &handler : client_handlers_) {
    if (handler->WantsLogEvents()) {
      client_wants_events = true;
      break;
    }
  }

  for (auto & [ timestamp, msg ] : log_buffer_) {
    toolbelt::LogLevel level;
    switch (msg->level()) {
    case adastra::proto::LogMessage::LOG_VERBOSE:
      level = toolbelt::LogLevel::kVerboseDebug;
      break;
    case adastra::proto::LogMessage::LOG_DBG:
      level = toolbelt::LogLevel::kDebug;
      break;
    case adastra::proto::LogMessage::LOG_INFO:
      level = toolbelt::LogLevel::kInfo;
      break;
    case adastra::proto::LogMessage::LOG_WARNING:
      level = toolbelt::LogLevel::kWarning;
      break;
    case adastra::proto::LogMessage::LOG_ERR:
      level = toolbelt::LogLevel::kError;
      break;
    default:
      continue;
    }

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

    if (!client_wants_events) {
      logger_.Log(level, timestamp, msg->source(), msg->text());
    } else {
      // Send as log events to the clients.
      for (auto &handler : client_handlers_) {
        if (absl::Status status = handler->SendLogEvent(msg); !status.ok()) {
          logger_.Log(toolbelt::LogLevel::kError,
                      "Failed to send log event: %s", strerror(errno));
        }
      }
    }
  }
  log_buffer_.clear();
}

void Capcom::LoggerFlushCoroutine(co::Coroutine *c) {
  for (;;) {
    c->Millisleep(500); // Flush the log buffer every 500ms.
    FlushLogs();
  }
}

absl::Status Capcom::Run() {
  logger_.Log(toolbelt::LogLevel::kInfo, "Capcom running on address %s",
              addr_.ToString().c_str());

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

  for (auto &client : client_handlers_) {
    client->FlushEvents(nullptr);
  }
  FlushLogs();

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

void Capcom::SendParameterUpdateEvent(const std::string &name,
                                      const parameters::Value &value) {
  for (auto &handler : client_handlers_) {
    if (absl::Status status = handler->SendParameterUpdateEvent(name, value);
        !status.ok()) {
      logger_.Log(toolbelt::LogLevel::kError,
                  "Failed to send parameter event to client %s: %s",
                  handler->GetClientName().c_str(), status.ToString().c_str());
    }
  }
}

void Capcom::SendParameterDeleteEvent(const std::vector<std::string> &names) {
  for (auto &handler : client_handlers_) {
    for (auto &name : names) {
      if (absl::Status status = handler->SendParameterDeleteEvent(name);
          !status.ok()) {
        logger_.Log(toolbelt::LogLevel::kError,
                    "Failed to send parameter event to client %s: %s",
                    handler->GetClientName().c_str(),
                    status.ToString().c_str());
      }
    }
  }
}

void Capcom::SendTelemetryEvent(
    const std::string &subsystem,
    const adastra::stagezero::control::TelemetryEvent &event) {
  adastra::proto::TelemetryEvent capcom_event;
  capcom_event.set_subsystem(subsystem);
  capcom_event.mutable_telemetry()->CopyFrom(event.telemetry());
  capcom_event.set_process_id(event.process_id());
  capcom_event.set_timestamp(event.timestamp());
  for (auto &handler : client_handlers_) {
    if (absl::Status s = handler->SendTelemetryEvent(capcom_event); !s.ok()) {
      logger_.Log(toolbelt::LogLevel::kError,
                  "Failed to send telemetry status event to client %s: %s",
                  handler->GetClientName().c_str(), s.ToString().c_str());
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

absl::Status Capcom::Abort(const std::string &reason, bool emergency,
                           co::Coroutine *c) {
  // First take the subsystems offline with an abort.  This will not
  // kill any running processes.
  absl::Status result = absl::OkStatus();

  emergency_aborting_ = emergency;

  for (auto & [ name, subsys ] : subsystems_) {
    if (subsys->IsCritical()) {
      continue;
    }
    auto msg = std::make_shared<Message>(
        Message{.code = Message::Code::kAbort, .emergency_abort = emergency});
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
      if (!subsys->IsCritical() && !subsys->IsOffline()) {
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
  for (auto & [ _, umbilical ] : stagezero_umbilicals_) {
    if (!umbilical.IsConnected()) {
      continue;
    }

    absl::Status status = umbilical.GetClient()->Abort(reason, emergency, c);
    if (!status.ok()) {
      result = absl::InternalError(
          absl::StrFormat("Failed to abort compute %s: %s",
                          umbilical.GetCompute()->name, status.ToString()));
    }
  }
  if (emergency) {
    // Spawn a coroutine to do the shutdown so that we respond correctly
    // to the request.
    AddCoroutine(std::make_unique<co::Coroutine>(
        co_scheduler_, [this, reason](co::Coroutine *c2) {
          std::string text = absl::StrFormat(
              "Capcom shutting down in an emergency abort: %s", reason);
          SendAlarm({.name = "Capcom",
                     .type = Alarm::Type::kSystem,
                     .severity = Alarm::Severity::kCritical,
                     .reason = Alarm::Reason::kEmergencyAbort,
                     .status = Alarm::Status::kRaised,
                     .details = text});

          c2->Sleep(1);
          logger_.Log(toolbelt::LogLevel::kFatal, "%s", text.c_str());
        }));
  }

  return result;
}

absl::Status Capcom::AddGlobalVariable(const Variable &var, co::Coroutine *c) {
  global_symbols_.AddSymbol(var.name, var.value, var.exported);
  // Send the global variable to all the stagezeros.
  for (auto & [ _, umbilical ] : stagezero_umbilicals_) {
    if (!umbilical.IsConnected()) {
      continue;
    } 

    if (absl::Status status = umbilical.GetClient()->SetGlobalVariable(
            var.name, var.value, var.exported, c);
        !status.ok()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to set global variable %s on compute %s: %s", var.name,
          umbilical.GetCompute()->name, status.ToString()));
    }
  }
  return absl::OkStatus();
}

absl::Status Capcom::PropagateParameterUpdate(const std::string &name,
                                              const parameters::Value &value,
                                              co::Coroutine *c) {
  absl::Status result = absl::OkStatus();

  for (auto & [ _, umbilical ] : stagezero_umbilicals_) {
    if (!umbilical.IsConnected()) {
      continue;
    } 


    absl::Status status = umbilical.GetClient()->SetParameter(name, value, c);
    if (!status.ok()) {
      result = absl::InternalError(
          absl::StrFormat("Failed to update parameter %s on compute %s: %s",
                          name, umbilical.GetCompute()->name, status.ToString()));
    }
  }

  return result;
}

absl::Status
Capcom::PropagateParameterDelete(const std::vector<std::string> &names,
                                 co::Coroutine *c) {

  absl::Status result = absl::OkStatus();

  for (auto & [ _, umbilical ] : stagezero_umbilicals_) {
     if (!umbilical.IsConnected()) {
      continue;
    } 


    absl::Status status = umbilical.GetClient()->DeleteParameters(names, c);
    if (!status.ok()) {
      // This will fail on the stagezero that deleted the parameter.
    }
  }

  return result;
}

absl::Status Capcom::SetParameter(const std::string &name,
                                  const parameters::Value &value) {
  if (absl::Status status = parameters_.SetParameter(name, value);
      !status.ok()) {
    return status;
  }
  SendParameterUpdateEvent(name, value);

  return PropagateParameterUpdate(name, value, nullptr);
}

absl::Status Capcom::DeleteParameters(const std::vector<std::string> &names,
                                      co::Coroutine *c) {
  if (names.empty()) {
    parameters_.Clear();
  } else {
    for (auto &name : names) {
      if (absl::Status status = parameters_.DeleteParameter(name);
          !status.ok()) {
        return status;
      }
    }
  }
  SendParameterDeleteEvent(names);

  return PropagateParameterDelete(names, c);
}

absl::StatusOr<std::vector<parameters::Parameter>>
Capcom::GetParameters(const std::vector<std::string> &names) {
  if (names.empty()) {
    return parameters_.GetAllParameters();
  }

  std::vector<parameters::Parameter> result;
  for (auto &name : names) {
    absl::StatusOr<parameters::Value> value = parameters_.GetParameter(name);
    if (!value.ok()) {
      return value.status();
    }
    result.push_back({name, *value});
  }
  return result;
}

absl::Status
Capcom::SetAllParameters(const std::vector<parameters::Parameter> &params) {
  for (auto &param : params) {
    if (absl::Status status = parameters_.SetParameter(param.name, param.value);
        !status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

absl::Status Capcom::HandleParameterEvent(
    const adastra::proto::parameters::ParameterEvent &event, co::Coroutine *c) {
  switch (event.event_case()) {
  case adastra::proto::parameters::ParameterEvent::kUpdate: {
    parameters::Value value;
    value.FromProto(event.update().value());
    if (absl::Status status = SetParameter(event.update().name(), value);
        !status.ok()) {
      return status;
    }
    break;
  }
  case adastra::proto::parameters::ParameterEvent::kDelete: {
    if (absl::Status status = DeleteParameters({event.delete_()}, c);
        !status.ok()) {
      return status;
    }
    break;
  }
  default:
    return absl::InternalError(
        absl::StrFormat("Unknown parameter event type %d", event.event_case()));
  }
  return absl::OkStatus();
}

void Capcom::Log(const std::string &source, toolbelt::LogLevel level,
                 const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char buffer[256];
  vsnprintf(buffer, sizeof(buffer), fmt, ap);
  LogMessage log = {.source = source, .level = level, .text = buffer};

  struct timespec now_ts;
  clock_gettime(CLOCK_REALTIME, &now_ts);
  uint64_t now_ns = now_ts.tv_sec * 1000000000LL + now_ts.tv_nsec;
  log.timestamp = now_ns;

  adastra::proto::LogMessage proto_msg;
  log.ToProto(&proto_msg);
  // std::cerr << proto_msg.DebugString() << std::endl;
  Log(proto_msg);
}

absl::Status
Capcom::RegisterComputeCgroups(std::shared_ptr<stagezero::Client> client,
                               std::shared_ptr<Compute> compute,
                               co::Coroutine *c) {
  for (auto &cgroup : compute->cgroups) {
    if (absl::Status status = client->RegisterCgroup(cgroup, c); !status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

static const Cgroup *FindCgroup(const Compute &comp,
                                const std::string &cgroup) {
  for (auto &cg : comp.cgroups) {
    if (cg.name == cgroup) {
      return &cg;
    }
  }
  return nullptr;
}

absl::Status Capcom::FreezeCgroup(const std::string &compute,
                                  const std::string &cgroup, co::Coroutine *c) {
  std::shared_ptr<Compute> comp = FindCompute(compute);
  if (comp == nullptr) {
    return absl::InternalError(absl::StrFormat("No such compute %s", compute));
  }
  const Cgroup *cg = FindCgroup(*comp, cgroup);
  if (cg == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No such cgroup %s on computer %s", cgroup, compute));
  }
  Umbilical *umbilical = FindUmbilical(compute);
  if (umbilical == nullptr || !umbilical->IsConnected()) {
    return absl::InternalError(
        absl::StrFormat("Not connected to compute %s", compute));
  }

  return umbilical->GetClient()->FreezeCgroup(cgroup, c);
}

absl::Status Capcom::ThawCgroup(const std::string &compute,
                                const std::string &cgroup, co::Coroutine *c) {
  std::shared_ptr<Compute> comp = FindCompute(compute);
  if (comp == nullptr) {
    return absl::InternalError(absl::StrFormat("No such compute %s", compute));
  }
  const Cgroup *cg = FindCgroup(*comp, cgroup);
  if (cg == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No such cgroup %s on computer %s", cgroup, compute));
  }

  Umbilical *umbilical = FindUmbilical(compute);
  if (umbilical == nullptr || !umbilical->IsConnected()) {
    return absl::InternalError(
        absl::StrFormat("Not connected to compute %s", compute));
  }

  return umbilical->GetClient()->ThawCgroup(cgroup, c);
}

absl::Status Capcom::KillCgroup(const std::string &compute,
                                const std::string &cgroup, co::Coroutine *c) {

  std::shared_ptr<Compute> comp = FindCompute(compute);
  if (comp == nullptr) {
    return absl::InternalError(absl::StrFormat("No such compute %s", compute));
  }
  const Cgroup *cg = FindCgroup(*comp, cgroup);
  if (cg == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No such cgroup %s on computer %s", cgroup, compute));
  }
  Umbilical *umbilical = FindUmbilical(compute);
  if (umbilical == nullptr || !umbilical->IsConnected()) {
    return absl::InternalError(
        absl::StrFormat("Not connected to compute %s", compute));
  }

  return umbilical->GetClient()->KillCgroup(cgroup, c);
}

absl::Status
Capcom::SendTelemetryCommand(const proto::SendTelemetryCommandRequest &req,
                             co::Coroutine *c) {
  switch (req.dest_case()) {
  case proto::SendTelemetryCommandRequest::kSubsystem: {
    auto it = subsystems_.find(req.subsystem());
    if (it == subsystems_.end()) {
      return absl::InternalError(
          absl::StrFormat("No such subsystem %s", req.subsystem()));
    }
    return it->second->SendTelemetryCommand(req.command(), c);
  }

  case proto::SendTelemetryCommandRequest::kProcess: {
    auto subsystem_name = req.process().subsystem();
    auto process_id = req.process().process_id();
    auto it = subsystems_.find(subsystem_name);
    if (it == subsystems_.end()) {
      return absl::InternalError(
          absl::StrFormat("No such subsystem %s", subsystem_name));
    }
    auto proc = it->second->FindProcess(process_id);
    if (proc == nullptr) {
      return absl::InternalError(absl::StrFormat(
          "No such process %s in subsystem %s", process_id, subsystem_name));
    }
    return proc->SendTelemetryCommand(it->second, req.command(), c);
  }
  default:
    return absl::InternalError(absl::StrFormat(
        "Unknown telemetry command destination %d", req.dest_case()));
  }
}
} // namespace adastra::capcom
