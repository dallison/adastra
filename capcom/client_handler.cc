// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "capcom/client_handler.h"
#include "absl/strings/str_format.h"
#include "capcom/capcom.h"
#include "common/stream.h"
#include "toolbelt/hexdump.h"
#include "toolbelt/pipe.h"

#include <iostream>

namespace stagezero::capcom {

ClientHandler::~ClientHandler() {}

co::CoroutineScheduler &ClientHandler::GetScheduler() const {
  return capcom_.co_scheduler_;
}

void ClientHandler::Shutdown() {}

toolbelt::Logger &ClientHandler::GetLogger() const { return capcom_.logger_; }

void ClientHandler::AddCoroutine(std::unique_ptr<co::Coroutine> c) {
  capcom_.AddCoroutine(std::move(c));
}

absl::Status ClientHandler::SendSubsystemStatusEvent(Subsystem *subsystem) {
  if ((event_mask_ & kSubsystemStatusEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<stagezero::proto::Event>();
  auto s = event->mutable_subsystem_status();
  subsystem->BuildStatus(s);
  return QueueEvent(std::move(event));
}

absl::Status ClientHandler::SendAlarm(const Alarm &alarm) {
  if ((event_mask_ & kAlarmEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<stagezero::proto::Event>();
  auto a = event->mutable_alarm();
  alarm.ToProto(a);
  return QueueEvent(std::move(event));
}

absl::Status
ClientHandler::SendLogEvent(std::shared_ptr<stagezero::proto::LogMessage> msg) {
  if ((event_mask_ & kLogMessageEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<stagezero::proto::Event>();
  auto log = event->mutable_log();
  *log = *msg; // This is a copy.
  return QueueEvent(std::move(event));
}

absl::Status ClientHandler::HandleMessage(const proto::Request &req,
                                          proto::Response &resp,
                                          co::Coroutine *c) {
  switch (req.request_case()) {
  case proto::Request::kInit:
    HandleInit(req.init(), resp.mutable_init(), c);
    break;
  case proto::Request::kAddCompute:
    HandleAddCompute(req.add_compute(), resp.mutable_add_compute(), c);
    break;

  case proto::Request::kRemoveCompute:
    HandleRemoveCompute(req.remove_compute(), resp.mutable_remove_compute(), c);
    break;

  case proto::Request::kAddSubsystem:
    HandleAddSubsystem(req.add_subsystem(), resp.mutable_add_subsystem(), c);
    break;

  case proto::Request::kRemoveSubsystem:
    HandleRemoveSubsystem(req.remove_subsystem(),
                          resp.mutable_remove_subsystem(), c);
    break;
  case proto::Request::kStartSubsystem:
    HandleStartSubsystem(req.start_subsystem(), resp.mutable_start_subsystem(),
                         c);
    break;
  case proto::Request::kStopSubsystem:
    HandleStopSubsystem(req.stop_subsystem(), resp.mutable_stop_subsystem(), c);
    break;
  case proto::Request::kGetSubsystems:
    HandleGetSubsystems(req.get_subsystems(), resp.mutable_get_subsystems(), c);
    break;
  case proto::Request::kGetAlarms:
    HandleGetAlarms(req.get_alarms(), resp.mutable_get_alarms(), c);
    break;

  case proto::Request::kAbort:
    HandleAbort(req.abort(), resp.mutable_abort(), c);
    break;

  case proto::Request::kInput:
    HandleInput(req.input(), resp.mutable_input(), c);
    break;

  case proto::Request::kCloseFd:
    HandleCloseFd(req.close_fd(), resp.mutable_close_fd(), c);
    break;

  case proto::Request::kAddGlobalVariable:
    HandleAddGlobalVariable(req.add_global_variable(),
                            resp.mutable_add_global_variable(), c);
    break;

  case proto::Request::REQUEST_NOT_SET:
    return absl::InternalError("Protocol error: unknown request");
  }
  return absl::OkStatus();
}

void ClientHandler::HandleInit(const proto::InitRequest &req,
                               proto::InitResponse *response,
                               co::Coroutine *c) {
  absl::StatusOr<int> s = Init(req.client_name(), req.event_mask(), [] {}, c);
  if (!s.ok()) {
    response->set_error(s.status().ToString());
    return;
  }
  response->set_event_port(*s);
}

void ClientHandler::HandleAddCompute(const proto::AddComputeRequest &req,
                                     proto::AddComputeResponse *response,
                                     co::Coroutine *c) {
  const config::Compute &compute = req.compute();
  struct sockaddr_in addr = {
#if defined(__APPLE__)
    .sin_len = sizeof(int),
#endif
    .sin_family = AF_INET,
    .sin_port = htons(compute.port()),
  };
  uint32_t ip_addr;

  memcpy(&ip_addr, compute.ip_addr().data(), compute.ip_addr().size());
  addr.sin_addr.s_addr = htonl(ip_addr);

  toolbelt::InetAddress stagezero_addr(addr);

  // Probe a connection to the stagezero instance to make sure it's
  // there.
  stagezero::Client sclient;
  if (absl::Status status = sclient.Init(stagezero_addr, "<capcom probe>",
                                         kNoEvents, compute.name(), c);
      !status.ok()) {
    response->set_error(absl::StrFormat(
        "Cannot connect to StageZero on compute %s at address %s",
        compute.name(), stagezero_addr.ToString()));
    return;
  }

  Compute c2 = {compute.name(), toolbelt::InetAddress(addr)};
  bool ok = capcom_.AddCompute(compute.name(), c2);
  if (!ok) {
    response->set_error(
        absl::StrFormat("Failed to add compute %s", compute.name()));
  }
}

void ClientHandler::HandleRemoveCompute(const proto::RemoveComputeRequest &req,
                                        proto::RemoveComputeResponse *response,
                                        co::Coroutine *c) {
  if (absl::Status status = capcom_.RemoveCompute(req.name()); !status.ok()) {
    response->set_error(
        absl::StrFormat("Failed to remove compute %s", req.name()));
  }
}

void ClientHandler::HandleAddSubsystem(const proto::AddSubsystemRequest &req,
                                       proto::AddSubsystemResponse *response,
                                       co::Coroutine *c) {
  // Validate the children.
  std::vector<std::shared_ptr<Subsystem>> children;
  children.reserve(req.children_size());
  for (auto &child_name : req.children()) {
    auto child = capcom_.FindSubsystem(child_name);
    if (child == nullptr) {
      response->set_error(
          absl::StrFormat("Unknown child subsystem %s", child_name));
      return;
    }
    children.push_back(child);
  }

  std::vector<Variable> vars;
  for (auto &var : req.vars()) {
    vars.push_back(
        {.name = var.name(), .value = var.value(), .exported = var.exported()});
  }
  std::vector<Stream> streams;
  for (auto &s : req.streams()) {
    Stream stream;
    if (absl::Status status = stream.FromProto(s); !status.ok()) {
      response->set_error(status.ToString());
      return;
    }
    streams.push_back(stream);
  }
  auto subsystem = std::make_shared<Subsystem>(
      req.name(), capcom_, std::move(vars), std::move(streams),
      req.max_restarts(), req.critical());

  // Add the processes to the subsystem.
  for (auto &proc : req.processes()) {
    const Compute *compute = capcom_.FindCompute(proc.compute());
    if (compute == nullptr) {
      response->set_error(absl::StrFormat(
          "No such compute %s for process %s (have you added it?)",
          proc.compute(), proc.options().name()));
      return;
    }

    if (absl::Status status = ValidateStreams(proc.streams()); !status.ok()) {
      response->set_error(status.ToString());
      return;
    }

    switch (proc.proc_case()) {
    case proto::Process::kStaticProcess:
      if (absl::Status status =
              subsystem->AddStaticProcess(proc.static_process(), proc.options(),
                                          proc.streams(), compute, c);
          !status.ok()) {
        response->set_error(
            absl::StrFormat("Failed to add static process %s: %s",
                            proc.options().name(), status.ToString()));
        return;
      }
      break;
    case proto::Process::kZygote:
      if (absl::Status status = subsystem->AddZygote(
              proc.zygote(), proc.options(), proc.streams(), compute, c);
          !status.ok()) {
        response->set_error(absl::StrFormat("Failed to add zygote %s: %s",
                                            proc.options().name(),
                                            status.ToString()));
        return;
      }
      break;
    case proto::Process::kVirtualProcess:
      if (absl::Status status = subsystem->AddVirtualProcess(
              proc.virtual_process(), proc.options(), proc.streams(), compute,
              c);
          !status.ok()) {
        response->set_error(
            absl::StrFormat("Failed to add virtual process %s: %s",
                            proc.options().name(), status.ToString()));
        return;
      }
      break;
    case proto::Process::PROC_NOT_SET:
      break;
    }
  }

  // All OK, add the subsystem now.
  if (!capcom_.AddSubsystem(req.name(), subsystem)) {
    response->set_error(absl::StrFormat(
        "Failed to add subsystem %s; already exists", req.name()));
    return;
  }

  // Link the children.
  for (auto child : children) {
    subsystem->AddChild(child);
    child->AddParent(subsystem);
  }

  // Start the subsystem running.  This spawns a coroutine and returns without
  // bloocking.
  subsystem->Run();
}

void ClientHandler::HandleRemoveSubsystem(
    const proto::RemoveSubsystemRequest &req,
    proto::RemoveSubsystemResponse *response, co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  if (!subsystem->CheckRemove(req.recursive())) {
    response->set_error("Cannot remove subsystems when they are online");
    return;
  }
  if (absl::Status status = subsystem->Remove(req.recursive()); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to remove subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
  }
}

void ClientHandler::HandleStartSubsystem(
    const proto::StartSubsystemRequest &req,
    proto::StartSubsystemResponse *response, co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }

  Message message = {.code = Message::kChangeAdmin,
                     .state.admin = AdminState::kOnline,
                     .client_id = id_,
                     .interactive = req.interactive()};

  if (message.interactive) {
    absl::StatusOr<toolbelt::Pipe> stdout = toolbelt::Pipe::Create();
    if (!stdout.ok()) {
      response->set_error(stdout.status().ToString());
      return;
    }
    // Put write end into message.
    message.output_fd = stdout->WriteFd().Fd();
    // Keep the write end of the pipe open as it will be passed to the subsystem
    // as a raw fd.  The subsystem will take ownership of the fd when it
    // receives the message from its message pipe.
    stdout->WriteFd().Release();

    if (req.has_terminal()) {
      message.rows = req.terminal().rows();
      message.cols = req.terminal().cols();
      if (req.terminal().name().size() > sizeof(message.term_name) - 1) {
        response->set_error(absl::StrFormat("Terminal name '%s' is too long",
                                            req.terminal().name()));
        return;
      }
      strcpy(message.term_name, req.terminal().name().c_str());
    }

    // Spawn coroutine to read from the stdout pipe
    // and send as output events.
    AddCoroutine(std::make_unique<co::Coroutine>(GetScheduler(), [
      client = shared_from_this(), stdout = stdout->ReadFd()
    ](co::Coroutine * c) {
      for (;;) {
        char buffer[4096];
        c->Wait(stdout.Fd());
        ssize_t n = ::read(stdout.Fd(), buffer, sizeof(buffer));
        if (n <= 0) {
          break;
        }
        if (absl::Status status =
                client->SendOutputEvent("", STDOUT_FILENO, buffer, n);
            !status.ok()) {
          break;
        }
      }
      client->Stop();
    }));
  }

  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to start subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
  }
}

void ClientHandler::HandleStopSubsystem(const proto::StopSubsystemRequest &req,
                                        proto::StopSubsystemResponse *response,
                                        co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  Message message = {.code = Message::kChangeAdmin,
                     .state.admin = AdminState::kOffline,
                     .client_id = id_};
  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to stop subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
    return;
  }
}

void ClientHandler::HandleGetSubsystems(const proto::GetSubsystemsRequest &req,
                                        proto::GetSubsystemsResponse *response,
                                        co::Coroutine *c) {
  std::vector<Subsystem *> subsystems = capcom_.GetSubsystems();
  for (auto subsystem : subsystems) {
    auto *s = response->add_subsystems();
    subsystem->BuildStatus(s);
  }
}

void ClientHandler::HandleGetAlarms(const proto::GetAlarmsRequest &req,
                                    proto::GetAlarmsResponse *response,
                                    co::Coroutine *c) {
  std::vector<Alarm> alarms = capcom_.GetAlarms();
  for (auto &alarm : alarms) {
    auto *a = response->add_alarms();
    alarm.ToProto(a);
  }
}

void ClientHandler::HandleAbort(const proto::AbortRequest &req,
                                proto::AbortResponse *response,
                                co::Coroutine *c) {
                                  std::cerr << "CAPCOM ABORT: " << req.DebugString();
  if (absl::Status status = capcom_.Abort(req.reason(), req.emergency(), c); !status.ok()) {
    response->set_error(status.ToString());
  }
}

absl::Status ClientHandler::SendOutputEvent(const std::string &process, int fd,
                                            const char *data, size_t len) {
  if ((event_mask_ & kOutputEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<stagezero::proto::Event>();
  auto output = event->mutable_output();
  output->set_process_id(process);
  output->set_data(data, len);
  output->set_fd(fd);
  return QueueEvent(std::move(event));
}

void ClientHandler::HandleAddGlobalVariable(
    const proto::AddGlobalVariableRequest &req,
    proto::AddGlobalVariableResponse *response, co::Coroutine *c) {
  Variable var = {.name = req.var().name(),
                  .value = req.var().value(),
                  .exported = req.var().exported()};
  if (absl::Status status = capcom_.AddGlobalVariable(std::move(var), c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleInput(const proto::InputRequest &req,
                                proto::InputResponse *response,
                                co::Coroutine *c) {
  auto subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  if (absl::Status status =
          subsystem->SendInput(req.process(), req.fd(), req.data(), c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleCloseFd(const proto::CloseFdRequest &req,
                                  proto::CloseFdResponse *response,
                                  co::Coroutine *c) {
  auto subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  if (absl::Status status = subsystem->CloseFd(req.process(), req.fd(), c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}
} // namespace stagezero::capcom
