// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "capcom/client_handler.h"
#include "absl/strings/str_format.h"
#include "capcom/capcom.h"
#include "common/stream.h"
#include "toolbelt/hexdump.h"
#include "toolbelt/pipe.h"

#include <iostream>

namespace adastra::capcom {

ClientHandler::ClientHandler(Capcom &capcom, toolbelt::TCPSocket socket,
                             uint32_t id)
    : TCPClientHandler(capcom.logger_, std::move(socket)), capcom_(capcom),
      id_(id) {}

ClientHandler::~ClientHandler() {}

co::CoroutineScheduler &ClientHandler::GetScheduler() const {
  return capcom_.co_scheduler_;
}

void ClientHandler::Shutdown() {}

void ClientHandler::AddCoroutine(std::unique_ptr<co::Coroutine> c) {
  capcom_.AddCoroutine(std::move(c));
}

absl::Status ClientHandler::SendSubsystemStatusEvent(Subsystem *subsystem) {
  if ((event_mask_ & kSubsystemStatusEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<adastra::proto::Event>();
  auto s = event->mutable_subsystem_status();
  subsystem->BuildStatus(s);
  return QueueEvent(std::move(event));
}

absl::Status
ClientHandler::SendParameterUpdateEvent(const std::string &name,
                                        const parameters::Value &value) {
  if ((event_mask_ & kParameterEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<adastra::proto::Event>();
  auto p = event->mutable_parameter();
  auto update = p->mutable_update();
  update->set_name(name);
  value.ToProto(update->mutable_value());
  return QueueEvent(std::move(event));
}

absl::Status ClientHandler::SendParameterDeleteEvent(const std::string &name) {
  if ((event_mask_ & kParameterEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<adastra::proto::Event>();
  auto p = event->mutable_parameter();
  p->set_delete_(name);
  return QueueEvent(std::move(event));
}

absl::Status
ClientHandler::SendTelemetryEvent(const adastra::proto::TelemetryEvent &event) {
  if ((event_mask_ & kTelemetryEvents) == 0) {
    return absl::OkStatus();
  }
  auto e = std::make_shared<adastra::proto::Event>();
  *e->mutable_telemetry() = event;
  return QueueEvent(std::move(e));
}

absl::Status ClientHandler::SendAlarm(const Alarm &alarm) {
  if ((event_mask_ & kAlarmEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<adastra::proto::Event>();
  auto a = event->mutable_alarm();
  alarm.ToProto(a);
  Log("capcom", toolbelt::LogLevel::kInfo, "Alarm: %s",
      a->DebugString().c_str());
  return QueueEvent(std::move(event));
}

absl::Status
ClientHandler::SendLogEvent(std::shared_ptr<adastra::proto::LogMessage> msg) {
  if ((event_mask_ & kLogMessageEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<adastra::proto::Event>();
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
  case proto::Request::kRestartSubsystem:
    HandleRestartSubsystem(req.restart_subsystem(),
                           resp.mutable_restart_subsystem(), c);
    break;
  case proto::Request::kRestartProcesses:
    HandleRestartProcesses(req.restart_processes(),
                           resp.mutable_restart_processes(), c);
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
  case proto::Request::kFreezeCgroup:
    HandleFreezeCgroup(req.freeze_cgroup(), resp.mutable_freeze_cgroup(), c);
    break;

  case proto::Request::kThawCgroup:
    HandleThawCgroup(req.thaw_cgroup(), resp.mutable_thaw_cgroup(), c);
    break;

  case proto::Request::kKillCgroup:
    HandleKillCgroup(req.kill_cgroup(), resp.mutable_kill_cgroup(), c);
    break;

  case proto::Request::kSetParameter:
    HandleSetParameter(req.set_parameter(), resp.mutable_set_parameter(), c);
    break;

  case proto::Request::kDeleteParameters:
    HandleDeleteParameters(req.delete_parameters(),
                           resp.mutable_delete_parameters(), c);
    break;

  case proto::Request::kGetParameters:
    HandleGetParameters(req.get_parameters(), resp.mutable_get_parameters(), c);
    break;

  case proto::Request::kUploadParameters:
    HandleUploadParameters(req.upload_parameters(),
                           resp.mutable_upload_parameters(), c);
    break;

  case proto::Request::kSendTelemetryCommand:
    HandleSendTelemetryCommand(req.send_telemetry_command(),
                               resp.mutable_send_telemetry_command(), c);
    break;

  case proto::Request::REQUEST_NOT_SET:
    return absl::InternalError("Protocol error: unknown request");
  }
  return absl::OkStatus();
}

void ClientHandler::HandleInit(const proto::InitRequest &req,
                               proto::InitResponse *response,
                               co::Coroutine *c) {
  absl::StatusOr<int> s =
      Init(req.client_name(), req.event_mask(),
           []() -> absl::Status { return absl::OkStatus(); }, c);
  if (!s.ok()) {
    response->set_error(s.status().ToString());
    return;
  }
  response->set_event_port(*s);
}

void ClientHandler::HandleAddCompute(const proto::AddComputeRequest &req,
                                     proto::AddComputeResponse *response,
                                     co::Coroutine *c) {
  const stagezero::config::Compute &compute = req.compute();
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

  std::vector<Cgroup> cgroups;
  for (auto &cg : compute.cgroups()) {
    Cgroup cgroup;
    cgroup.FromProto(cg);
    cgroups.push_back(std::move(cgroup));
  }
  Compute c2 = {compute.name(), toolbelt::InetAddress(addr),
                std::move(cgroups)};
  auto[compute_ptr, ok] = capcom_.AddCompute(compute.name(), c2);
  if (!ok) {
    response->set_error(
        absl::StrFormat("Failed to add compute %s", compute.name()));
    return;
  }

  // If this is a static compute connection, connect the umbilical now and keep
  // it open.
  if (req.connection_policy() == proto::AddComputeRequest::STATIC) {
    capcom_.AddUmbilical(compute_ptr, true);
    if (absl::Status status = capcom_.ConnectUmbilical(compute.name(), c);
        !status.ok()) {
      response->set_error(absl::StrFormat("Failed to connect to compute %s: %s",
                                          compute.name(), status.ToString()));
    }
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
  Subsystem::RestartPolicy restart_policy;
  switch (req.restart_policy()) {
  case adastra::capcom::proto::AddSubsystemRequest::AUTOMATIC:
  default:
    restart_policy = Subsystem::RestartPolicy::kAutomatic;
    break;
  case adastra::capcom::proto::AddSubsystemRequest::MANUAL:
    restart_policy = Subsystem::RestartPolicy::kManual;
    break;
  case proto::AddSubsystemRequest::PROCESS_ONLY:
    restart_policy = Subsystem::RestartPolicy::kProcessOnly;
    break;
  }
  auto subsystem = std::make_shared<Subsystem>(
      req.name(), capcom_, std::move(vars), std::move(streams),
      req.max_restarts(), req.critical(), restart_policy);

  // Add the processes to the subsystem.
  for (auto &proc : req.processes()) {
    const std::shared_ptr<Compute> compute =
        capcom_.FindCompute(proc.compute());
    if (compute == nullptr) {
      response->set_error(absl::StrFormat(
          "No such compute %s for process %s (have you added it?)",
          proc.compute(), proc.options().name()));
      return;
    }

    // Record a reference to an umbilical for the compute.  It will not be
    // connected until needed.
    subsystem->AddUmbilicalReference(compute);

    if (absl::Status status = ValidateStreams(proc.streams()); !status.ok()) {
      response->set_error(status.ToString());
      return;
    }

    switch (proc.proc_case()) {
    case proto::Process::kStaticProcess:
      if (absl::Status status = subsystem->AddStaticProcess(
              proc.static_process(), proc.options(), proc.streams(),
              compute->name, proc.max_restarts(), c);
          !status.ok()) {
        response->set_error(
            absl::StrFormat("Failed to add static process %s: %s",
                            proc.options().name(), status.ToString()));
        return;
      }
      break;
    case proto::Process::kZygote:
      if (absl::Status status = subsystem->AddZygote(
              proc.zygote(), proc.options(), proc.streams(), compute->name,
              proc.max_restarts(), c);
          !status.ok()) {
        response->set_error(absl::StrFormat("Failed to add zygote %s: %s",
                                            proc.options().name(),
                                            status.ToString()));
        return;
      }
      break;
    case proto::Process::kVirtualProcess:
      if (absl::Status status = subsystem->AddVirtualProcess(
              proc.virtual_process(), proc.options(), proc.streams(),
              compute->name, proc.max_restarts(), c);
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

  auto message =
      std::make_shared<Message>(Message{.code = Message::kChangeAdmin,
                                        .client_id = id_,
                                        .state = {.admin = AdminState::kOnline},
                                        .interactive = req.interactive()});
  if (message->interactive) {
    absl::StatusOr<toolbelt::Pipe> stdout = toolbelt::Pipe::Create();
    if (!stdout.ok()) {
      response->set_error(stdout.status().ToString());
      return;
    }
    // Put write end into message.
    message->output_fd = stdout->WriteFd().Fd();
    // Keep the write end of the pipe open as it will be passed to the subsystem
    // as a raw fd.  The subsystem will take ownership of the fd when it
    // receives the message from its message pipe.
    stdout->WriteFd().Release();

    if (req.has_terminal()) {
      message->rows = req.terminal().rows();
      message->cols = req.terminal().cols();
      message->term_name = req.terminal().name();
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
  auto message = std::make_shared<Message>(
      Message{.code = Message::kChangeAdmin,
              .client_id = id_,
              .state = {.admin = AdminState::kOffline}});
  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to stop subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
    return;
  }
}

void ClientHandler::HandleRestartSubsystem(
    const proto::RestartSubsystemRequest &req,
    proto::RestartSubsystemResponse *response, co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  auto message = std::make_shared<Message>(
      Message{.code = Message::kRestart, .client_id = id_});

  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to restart subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
    return;
  }
}

void ClientHandler::HandleRestartProcesses(
    const proto::RestartProcessesRequest &req,
    proto::RestartProcessesResponse *response, co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  auto message = std::make_shared<Message>(
      Message{.code = Message::kRestartProcesses, .client_id = id_});
  for (auto &process : req.processes()) {
    std::shared_ptr<Process> p = subsystem->FindProcessName(process);
    if (p == nullptr) {
      response->set_error(absl::StrFormat("No such process %s in subsystem %s",
                                          process, req.subsystem()));
      return;
    }
    message->processes.push_back(p);
  }
  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(
        absl::StrFormat("Failed to restart processes in subsystem %s: %s",
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
  if (absl::Status status = capcom_.Abort(req.reason(), req.emergency(), c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

absl::Status ClientHandler::SendOutputEvent(const std::string &process, int fd,
                                            const char *data, size_t len) {
  if ((event_mask_ & kOutputEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<adastra::proto::Event>();
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
void ClientHandler::HandleFreezeCgroup(const proto::FreezeCgroupRequest &req,
                                       proto::FreezeCgroupResponse *response,
                                       co::Coroutine *c) {
  if (absl::Status status =
          capcom_.FreezeCgroup(req.compute(), req.cgroup(), c);
      !status.ok()) {
    response->set_error(absl::StrFormat("Failed to freeze cgroup %s: %s",
                                        req.cgroup(), status.ToString()));
  }
}

void ClientHandler::HandleThawCgroup(const proto::ThawCgroupRequest &req,
                                     proto::ThawCgroupResponse *response,
                                     co::Coroutine *c) {

  if (absl::Status status = capcom_.ThawCgroup(req.compute(), req.cgroup(), c);
      !status.ok()) {
    response->set_error(absl::StrFormat("Failed to freeze cgroup %s: %s",
                                        req.cgroup(), status.ToString()));
  }
}

void ClientHandler::HandleKillCgroup(const proto::KillCgroupRequest &req,
                                     proto::KillCgroupResponse *response,
                                     co::Coroutine *c) {
  if (absl::Status status = capcom_.KillCgroup(req.compute(), req.cgroup(), c);
      !status.ok()) {
    response->set_error(absl::StrFormat("Failed to freeze cgroup %s: %s",
                                        req.cgroup(), status.ToString()));
  }
}

void ClientHandler::HandleSetParameter(const proto::SetParameterRequest &req,
                                       proto::SetParameterResponse *response,
                                       co::Coroutine *c) {
  parameters::Value v;
  v.FromProto(req.value());
  if (absl::Status status = capcom_.SetParameter(req.name(), v); !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleDeleteParameters(
    const proto::DeleteParametersRequest &req,
    proto::DeleteParametersResponse *response, co::Coroutine *c) {
  std::vector<std::string> names;
  for (auto &name : req.names()) {
    names.push_back(name);
  }
  if (absl::Status status = capcom_.DeleteParameters(names, c); !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleGetParameters(const proto::GetParametersRequest &req,
                                        proto::GetParametersResponse *response,
                                        co::Coroutine *c) {
  std::vector<std::string> names;
  for (auto &name : req.names()) {
    names.push_back(name);
  }
  absl::StatusOr<std::vector<parameters::Parameter>> status =
      capcom_.GetParameters(names);
  if (!status.ok()) {
    response->set_error(status.status().ToString());
    return;
  }
  for (auto &p : status.value()) {
    auto *param = response->add_parameters();
    param->set_name(p.name);
    p.value.ToProto(param->mutable_value());
  }
}

void ClientHandler::HandleUploadParameters(
    const proto::UploadParametersRequest &req,
    proto::UploadParametersResponse *response, co::Coroutine *c) {
  std::vector<parameters::Parameter> params;

  for (auto &p : req.parameters()) {
    parameters::Value v;
    v.FromProto(p.value());
    params.push_back({p.name(), std::move(v)});
  }

  if (absl::Status status = capcom_.SetAllParameters(params); !status.ok()) {
    response->set_error(status.ToString());
    return;
  }
}

void ClientHandler::HandleSendTelemetryCommand(
    const proto::SendTelemetryCommandRequest &req,
    proto::SendTelemetryCommandResponse *response, co::Coroutine *c) {
  if (absl::Status status = capcom_.SendTelemetryCommand(req, c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}
} // namespace adastra::capcom
