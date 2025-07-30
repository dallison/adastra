// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "capcom/subsystem.h"
#include "capcom/capcom.h"

#include <unistd.h>

namespace adastra::capcom {
Subsystem::Subsystem(std::string name, Capcom &capcom,
                     std::vector<Variable> vars, std::vector<Stream> streams,
                     int max_restarts, bool critical,
                     RestartPolicy restart_policy)
    : name_(std::move(name)), capcom_(capcom), vars_(std::move(vars)),
      streams_(std::move(streams)), max_restarts_(max_restarts),
      critical_(critical), restart_policy_(restart_policy) {
  // Build the message pipe.
  if (absl::Status status = BuildMessagePipe(); !status.ok()) {
    capcom_.Log(Name(), toolbelt::LogLevel::kError, "%s",
                status.ToString().c_str());
  }
  // Open the interrupt trigger.
  if (absl::Status status = interrupt_.Open(); !status.ok()) {
    capcom_.Log(Name(), toolbelt::LogLevel::kError,
                "Failed to open triggerfd: %s", status.ToString().c_str());
  }
}

std::shared_ptr<Compute> Subsystem::FindCompute(const std::string &name) const {
  return capcom_.FindCompute(name);
}

void Subsystem::ConnectUmbilical(const std::string &compute) {
  Umbilical *umbilical = FindUmbilical(compute);
  if (umbilical == nullptr) {
    capcom_.Log(Name(), toolbelt::LogLevel::kError,
                "No umbilical found for compute %s", compute.c_str());
    return;
  }
  bool was_closed = umbilical->Precondition();
  if (!was_closed) {
    // Already connecting or connected.
    return;
  }

  // Spawn a coroutine to connect to the stagezero instance via the client.
  // When the client connects a 'connect' event will be received.
  capcom_.AddCoroutine(std::make_unique<co::Coroutine>(capcom_.co_scheduler_, [
    subsystem = shared_from_this(), umbilical, compute
  ](co::Coroutine * c) {
    while (subsystem->IsConnecting()) {
      // Connect the umbilical to the stagezero client.
      if (absl::Status status =
              subsystem->capcom_.ConnectUmbilical(compute, c);
          !status.ok()) {
        subsystem->capcom_.Log(subsystem->Name(), toolbelt::LogLevel::kError,
                               "%s", status.ToString().c_str());
        c->Sleep(1);
        continue;
      }

      if (absl::Status status = umbilical->Connect(kAllEvents, c);
          !status.ok()) {
        subsystem->capcom_.Log(subsystem->Name(), toolbelt::LogLevel::kError,
                               "Failed to connect umbilical to %s: %s",
                               compute.c_str(), status.ToString().c_str());
        c->Sleep(1);
        continue;
      }

      subsystem->Wakeup();
      return;
    }
  }));
}

void Subsystem::DisconnectUmbilical(const std::string &compute) {
  Umbilical *umbilical = FindUmbilical(compute);
  if (umbilical == nullptr) {
    capcom_.Log(Name(), toolbelt::LogLevel::kError,
                "No umbilical found for compute %s", compute.c_str());
    return;
  }

  umbilical->Disconnect();
  capcom_.DisconnectUmbilical(compute, true);
}

void Subsystem::AddUmbilicalReference(std::shared_ptr<Compute> compute) {
  auto it = umbilicals_.find(compute->name);
  if (it == umbilicals_.end()) {
    umbilicals_.insert(
        {compute->name, Umbilical(Name(), capcom_.logger_, compute,
                                  std::make_shared<stagezero::Client>())});
    it = umbilicals_.find(compute->name);
    capcom_.AddUmbilical(compute, false);
  }
  it->second.IncStaticRefs(+1);
}

void Subsystem::RemoveUmbilicalReference(const std::string &compute) {
  auto it = umbilicals_.find(compute);
  if (it == umbilicals_.end()) {
    return;
  }
  it->second.IncStaticRefs(-1);
  if (it->second.HasStaticRefs()) {
    return;
  }
  umbilicals_.erase(it);
  // Remove the main umbilical if it's not static.
  capcom_.RemoveUmbilical(compute, true);
}

Umbilical *Subsystem::FindUmbilical(const std::string &compute) {
  auto it = umbilicals_.find(compute);
  if (it == umbilicals_.end()) {
    return nullptr;
  }
  return &it->second;
}

co::CoroutineScheduler &Subsystem::Scheduler() { return capcom_.co_scheduler_; }

void Subsystem::DisconnectProcessesForCompute(const std::string &compute) {
  auto subsystem = shared_from_this();
  // Find all processes on this compute and disconnect them from the umbilical.
  for (auto &proc : processes_) {
    if (proc->IsOnCompute(compute)) {
      proc->Disconnect(subsystem);
    }
  }
}

absl::Status
Subsystem::SendTelemetryCommand(const adastra::proto::telemetry::Command &cmd,
                                co::Coroutine *c) {

  // Send the kSendTelemetryCommand message to the subsystem.
  auto message = std::make_shared<Message>(
      Message{.code = Message::kSendTelemetryCommand,
              .sender = this,
              .telemetry_command =
                  std::make_shared<adastra::proto::telemetry::Command>(cmd)});
  if (absl::Status status = SendMessage(message); !status.ok()) {
    return absl::InternalError(
        absl::StrFormat("Failed to send telemetry command to subsystem %s: %s",
                        name_, status.ToString()));
  }
  return absl::OkStatus();
}

absl::Status
Subsystem::PropagateTelemetryCommandMessage(std::shared_ptr<Message> message,
                                            co::Coroutine *c) {
  for (auto &child : children_) {
    if (absl::Status status = child->SendMessage(message); !status.ok()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to send telemetry command to subsystem %s: %s", child->Name(),
          status.ToString()));
    }
  }
  return absl::OkStatus();
}

void Subsystem::Run() {
  // Run the subsystem in offline state.
  running_ = true;
  admin_state_ = AdminState::kOffline;
  prev_oper_state_ = OperState::kOffline;
  EnterState(OperState::kOffline, kNoClient);
  capcom_.Log(Name(), toolbelt::LogLevel::kDebug,
              "Subsystem %s is now ready to receive commands", Name().c_str());
}

void Subsystem::Stop() {
  running_ = false;
  interrupt_.Trigger();
}

bool Subsystem::CheckRemove(bool recursive) {
  if (recursive) {
    for (auto &child : children_) {
      if (!child->CheckRemove(recursive)) {
        return false;
      }
    }
  }
  if (admin_state_ != AdminState::kOffline ||
      oper_state_ != OperState::kOffline) {
    return false;
  }
  return true;
}

absl::Status Subsystem::Remove(bool recursive) {
  auto subsys = shared_from_this();
  if (recursive) {
    for (auto &child : subsys->children_) {
      if (absl::Status status = child->Remove(recursive); !status.ok()) {
        return status;
      }
    }
  }

  // Remove processes.
  subsys->process_map_.clear();
  for (auto &proc : subsys->processes_) {
    if (proc->IsZygote()) {
      if (absl::Status status = subsys->capcom_.RemoveZygote(proc->Name());
          !status.ok()) {
        return status;
      }
    }
    RemoveUmbilicalReference(proc->GetCompute());
  }
  if (absl::Status status = subsys->capcom_.RemoveSubsystem(subsys->Name());
      !status.ok()) {
    return status;
  }

  // Remove the parent/child linkages.
  for (auto &child : subsys->children_) {
    if (absl::Status status = child->RemoveParent(subsys.get()); !status.ok()) {
      return status;
    }
  }
  subsys->children_.clear();
  subsys->Stop(); // Stop the state coroutine running.
  return absl::OkStatus();
}

absl::Status Subsystem::BuildMessagePipe() {
  absl::StatusOr<toolbelt::SharedPtrPipe<Message>> p =
      toolbelt::SharedPtrPipe<Message>::Create();
  if (!p.ok()) {
    return p.status();
  }
  message_pipe_ = std::move(*p);
  return absl::OkStatus();
}

absl::Status Subsystem::SendMessage(std::shared_ptr<Message> message) {
  absl::Status status = message_pipe_.Write(message);
  if (!status.ok()) {
    return absl::InternalError(
        absl::StrFormat("Failed to send message to subsystem %s: %s", name_,
                        status.ToString()));
  }
  return absl::OkStatus();
}

void Subsystem::NotifyParents() {
  auto message =
      std::make_shared<Message>(Message{.code = Message::kReportOper,
                                        .sender = this,
                                        .state = {.oper = oper_state_}});
  for (auto &parent : parents_) {
    if (absl::Status status = parent->SendMessage(message); !status.ok()) {
      capcom_.Log(Name(), toolbelt::LogLevel::kError,
                  "Unable to notify parent %s of oper state change for "
                  "subsystem %s: %s",
                  parent->Name().c_str(), Name().c_str(),
                  status.ToString().c_str());
    }
  }
}

absl::Status Subsystem::SendInput(const std::string &process, int fd,
                                  const std::string &data, co::Coroutine *c) {
  std::shared_ptr<Process> proc = FindProcessName(process);
  if (proc == nullptr) {
    return absl::InternalError(absl::StrFormat("No such process %s", process));
  }
  Umbilical *umbilical = FindUmbilical(proc->GetCompute());
  if (umbilical == nullptr) {
    return absl::InternalError(absl::StrFormat(
        "No umbilical found for compute %s", proc->GetCompute()));
  }
  return proc->SendInput(umbilical, fd, data, c);
}

absl::Status Subsystem::CloseFd(const std::string &process, int fd,
                                co::Coroutine *c) {
  std::shared_ptr<Process> proc = FindProcessName(process);
  if (proc == nullptr) {
    return absl::InternalError(absl::StrFormat("No such process %s", process));
  }
  Umbilical *umbilical = FindUmbilical(proc->GetCompute());
  if (umbilical == nullptr) {
    return absl::InternalError(absl::StrFormat(
        "No umbilical found for compute %s", proc->GetCompute()));
  }
  return proc->CloseFd(umbilical, fd, c);
}

void Subsystem::SendToChildren(AdminState state, uint32_t client_id) {
  auto message =
      std::make_shared<Message>(Message{.code = Message::kChangeAdmin,
                                        .sender = this,
                                        .client_id = client_id,
                                        .state = {.admin = state}});
  for (auto &child : children_) {
    if (absl::Status status = child->SendMessage(message); !status.ok()) {
      capcom_.Log(Name(), toolbelt::LogLevel::kError,
                  "Unable to send admin state to %s for "
                  "subsystem %s: %s",
                  child->Name().c_str(), Name().c_str(),
                  status.ToString().c_str());
    }
  }
}

absl::StatusOr<std::shared_ptr<Message>> Subsystem::ReadMessage() {
  auto msg_or_status = message_pipe_.Read();
  if (!msg_or_status.ok()) {
    return absl::InternalError(
        absl::StrFormat("Failed to read message in subsystem %s: %s", name_,
                        msg_or_status.status().ToString()));
  }
  return *msg_or_status;
}

absl::Status Subsystem::LaunchProcesses(co::Coroutine *c) {
  for (auto &proc : processes_) {
    if (proc->IsRunning()) {
      continue;
    }
    proc->SetExit(false, -1);

    std::cerr << "Launching process " << proc->Name() << std::endl;
    absl::Status status = proc->Launch(this, c);
    if (!status.ok()) {
      // A failure to launch one is a failure for all.
      return status;
    }
    RecordProcessId(proc->GetProcessId(), proc);
  }
  return absl::OkStatus();
}

void Subsystem::StopProcesses(co::Coroutine *c) {
  for (auto &proc : processes_) {
    if (!proc->IsRunning()) {
      // Proc isn't running, but it might be connected.
      proc->Disconnect(shared_from_this());
      continue;
    }
    absl::Status status = proc->Stop(this, c);
    if (!status.ok()) {
      capcom_.Log(Name(), toolbelt::LogLevel::kError,
                  "Failed to stop process %s: %s", proc->Name().c_str(),
                  status.ToString().c_str());
      continue;
    }
  }
}

void Subsystem::ResetProcessRestarts() {
  for (auto &proc : processes_) {
    proc->ResetNumRestarts();
  }
}

void Subsystem::RestartProcesses(
    const std::vector<std::shared_ptr<Process>> &processes, co::Coroutine *c) {
  processes_to_restart_.clear();
  if (processes.empty()) {
    // Restart all processes.
    processes_to_restart_ = processes_;
    capcom_.Log(Name(), toolbelt::LogLevel::kInfo,
                "Restarting all processes in subsystem %s", Name().c_str());
    StopProcesses(c);
    return;
  }

  for (auto &proc : processes) {
    processes_to_restart_.push_back(proc);
    // Tell the user that we are restarting the process.
    capcom_.Log(Name(), toolbelt::LogLevel::kInfo,
                "Restarting process %s in subsystem %s", proc->Name().c_str(),
                Name().c_str());
    if (!proc->IsRunning()) {
      continue;
    }
    absl::Status status = proc->Stop(this, c);
    if (!status.ok()) {
      capcom_.Log(Name(), toolbelt::LogLevel::kError,
                  "Failed to stop process %s: %s", proc->Name().c_str(),
                  status.ToString().c_str());
      continue;
    }
  }
}

void Subsystem::SendOutput(int fd, const std::string& name, 
  const std::string& process_id, const std::string &data, co::Coroutine *c) {
  if (!interactive_output_.Valid()) {
    capcom_.SendOutputEvent(fd, name, process_id, data);
    return;
  }
  c->Wait(interactive_output_.Fd(), POLLOUT);
  int e = ::write(interactive_output_.Fd(), data.data(), data.size());
  if (e <= 0) {
    capcom_.Log(Name(), toolbelt::LogLevel::kDebug,
                "Failed to send process output: %s", strerror(errno));
  }
}

std::shared_ptr<Process> Subsystem::FindInteractiveProcess() { return nullptr; }

void Subsystem::RaiseAlarm(const Alarm &alarm) {
  alarm_ = alarm;
  alarm_.id = absl::StrFormat("%s/%d", alarm.name, alarm.type);
  capcom_.SendAlarm(alarm_);
  alarm_raised_ = true;
  alarm_count_++;
}

void Subsystem::ClearAlarm() {
  if (!alarm_raised_) {
    return;
  }
  alarm_.status = Alarm::Status::kCleared;
  capcom_.SendAlarm(alarm_);
  alarm_raised_ = false;
}

absl::Status Subsystem::AddStaticProcess(
    const stagezero::config::StaticProcess &proc,
    const stagezero::config::ProcessOptions &options,
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams,
    const std::string &compute, int max_restarts, co::Coroutine *c) {
  if (proc.executable().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Missing executable for static process %s", options.name()));
  }

  auto p = std::make_unique<StaticProcess>(capcom_, options.name(), compute,
                                           proc.executable(), options, streams);
  p->SetMaxRestarts(max_restarts);
  AddProcess(std::move(p));

  return absl::OkStatus();
}

absl::Status Subsystem::AddZygote(
    const stagezero::config::StaticProcess &proc,
    const stagezero::config::ProcessOptions &options,
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams,
    const std::string &compute, int max_restarts, co::Coroutine *c) {
  if (proc.executable().empty()) {
    return absl::InternalError(
        absl::StrFormat("Missing executable for zygote %s", options.name()));
  }

  Zygote *z = FindZygote(options.name());
  if (z != nullptr) {
    return absl::InternalError(
        absl::StrFormat("Zygote %s already exists", options.name()));
  }

  auto p = std::make_unique<Zygote>(capcom_, options.name(), compute,
                                    proc.executable(), options, streams);
  p->SetMaxRestarts(max_restarts);
  capcom_.AddZygote(options.name(), p.get());

  AddProcess(std::move(p));
  return absl::OkStatus();
}

Zygote *Subsystem::FindZygote(const std::string &name) {
  return capcom_.FindZygote(name);
}

absl::Status Subsystem::AddVirtualProcess(
    const stagezero::config::VirtualProcess &proc,
    const stagezero::config::ProcessOptions &options,
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams,
    const std::string &compute, int max_restarts, co::Coroutine *c) {
  if (proc.zygote().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Missing zygote for virtual process %s", options.name()));
  }
  // dso can be empty.
  if (proc.main_func().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Missing main_func for virtual process %s", options.name()));
  }
  Zygote *z = FindZygote(proc.zygote());
  if (z == nullptr) {
    return absl::InternalError(
        absl::StrFormat("Zygote %s doesn't exist for virtual process %s",
                        proc.zygote(), options.name()));
  }

  auto p = std::make_unique<VirtualProcess>(capcom_, options.name(), compute,
                                            proc.zygote(), proc.dso(),
                                            proc.main_func(), options, streams);
  p->SetMaxRestarts(max_restarts);
  AddProcess(std::move(p));

  return absl::OkStatus();
}

void Subsystem::BuildStatus(adastra::proto::SubsystemStatus *status) {
  status->set_name(name_);

  switch (admin_state_) {
  case AdminState::kOffline:
    status->set_admin_state(adastra::proto::ADMIN_OFFLINE);
    break;
  case AdminState::kOnline:
    status->set_admin_state(adastra::proto::ADMIN_ONLINE);
    break;
  }

  switch (oper_state_) {
  case OperState::kOffline:
    status->set_oper_state(adastra::proto::OPER_OFFLINE);
    break;
  case OperState::kStartingChildren:
    status->set_oper_state(adastra::proto::OPER_STARTING_CHILDREN);
    break;
  case OperState::kConnecting:
    status->set_oper_state(adastra::proto::OPER_CONNECTING);
    break;
  case OperState::kStartingProcesses:
    status->set_oper_state(adastra::proto::OPER_STARTING_PROCESSES);
    break;
  case OperState::kOnline:
    status->set_oper_state(adastra::proto::OPER_ONLINE);
    break;
  case OperState::kStoppingChildren:
    status->set_oper_state(adastra::proto::OPER_STOPPING_CHILDREN);
    break;
  case OperState::kStoppingProcesses:
    status->set_oper_state(adastra::proto::OPER_STOPPING_PROCESSES);
    break;
  case OperState::kRestarting:
    status->set_oper_state(adastra::proto::OPER_RESTARTING);
    break;
  case OperState::kRestartingProcesses:
    status->set_oper_state(adastra::proto::OPER_RESTARTING_PROCESSES);
    break;
  case OperState::kBroken:
    status->set_oper_state(adastra::proto::OPER_BROKEN);
    break;
  case OperState::kDegraded:
    status->set_oper_state(adastra::proto::OPER_DEGRADED);
    break;
  }
  status->set_alarm_count(alarm_count_);
  status->set_restart_count(restart_count_);

  // Processes.
  for (auto &proc : processes_) {
    auto *p = status->add_processes();
    p->set_name(proc->Name());
    p->set_process_id(proc->GetProcessId());
    p->set_pid(proc->GetPid());
    p->set_running(proc->IsRunning());
    p->set_compute(proc->GetCompute());
    p->set_subsystem(name_);
    p->set_alarm_count(proc->AlarmCount());
    if (proc->IsZygote()) {
      p->set_type(adastra::proto::SubsystemStatus::ZYGOTE);
    } else if (proc->IsVirtual()) {
      p->set_type(adastra::proto::SubsystemStatus::VIRTUAL);
    } else {
      p->set_type(adastra::proto::SubsystemStatus::STATIC);
    }
  }
}

void Subsystem::CollectAlarms(std::vector<Alarm> &alarms) const {
  if (alarm_raised_) {
    alarms.push_back(alarm_);
  }
  for (auto &proc : processes_) {
    const Alarm *a = proc->GetAlarm();
    if (a != nullptr) {
      alarms.push_back(*a);
    }
  }
}

bool Process::IsOnCompute(const std::string &c) const { return compute_ == c; }

void Process::Connect(std::shared_ptr<Subsystem> subsystem) {
  if (maybe_connected_) {
    return;
  }
  maybe_connected_ = true;
  subsystem->ConnectUmbilical(compute_);
}

void Process::Disconnect(std::shared_ptr<Subsystem> subsystem) {
  if (!maybe_connected_) {
    return;
  }
  SetStopped();
  maybe_connected_ = false;
  subsystem->DisconnectUmbilical(compute_);
}

bool Process::IsConnected(Subsystem *subsystem) const {
  Umbilical *umbilical = subsystem->FindUmbilical(compute_);
  if (umbilical == nullptr) {
    return false;
  }
  return umbilical->IsConnected();
}

absl::Status Process::SendInput(Umbilical *umbilical, int fd,
                                const std::string &data, co::Coroutine *c) {
  return umbilical->GetClient()->SendInput(process_id_, fd, data, c);
}

absl::Status Process::CloseFd(Umbilical *umbilical, int fd, co::Coroutine *c) {
  return umbilical->GetClient()->CloseProcessFileDescriptor(process_id_, fd, c);
}

void Process::ParseOptions(const stagezero::config::ProcessOptions &options) {
  description_ = options.description();

  // Copy args.
  args_.resize(options.args_size());
  std::copy(options.args().begin(), options.args().end(), args_.begin());

  // Copy vars.
  for (auto &var : options.vars()) {
    vars_.push_back({var.name(), var.value(), var.exported()});
  }
  // Local parameters.
  for (auto &param : options.parameters()) {
    parameters::Value v;
    v.FromProto(param.value());
    if (absl::Status status = local_parameters_.SetParameter(param.name(), v);
        !status.ok()) {
      capcom_.Log(Name(), toolbelt::LogLevel::kError,
                  "Failed to set local parameter %s: %s", param.name().c_str(),
                  status.ToString().c_str());
    }
  }
  startup_timeout_secs_ = options.startup_timeout_secs();
  telemetry_shutdown_timeout_secs_ = options.telemetry_shutdown_timeout_secs();
  sigint_shutdown_timeout_secs_ = options.sigint_shutdown_timeout_secs();
  sigterm_shutdown_timeout_secs_ = options.sigterm_shutdown_timeout_secs();
  notify_ = options.notify();
  telemetry_ = options.telemetry();
  interactive_ = options.interactive();
  user_ = options.user();
  group_ = options.group();
  critical_ = options.critical();
  oneshot_ = options.oneshot();
  cgroup_ = options.cgroup();
}

void Process::ParseStreams(
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams) {
  for (auto &s : streams) {
    Stream stream;
    if (absl::Status status = stream.FromProto(s); !status.ok()) {
      capcom_.Log(Name(), toolbelt::LogLevel::kError,
                  "Failed to parse stream control: %s",
                  status.ToString().c_str());
      continue;
    }
    streams_.push_back(stream);
  }
}

absl::Status Process::Stop(Subsystem *subsystem, co::Coroutine *c) {
  num_restarts_ = 0;
  Umbilical *umbilical = subsystem->FindUmbilical(compute_);
  if (umbilical == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No umbilical found for compute %s", compute_));
  }
  return umbilical->GetClient()->StopProcess(process_id_, c);
}

void Process::RaiseAlarm(Capcom &capcom, const Alarm &alarm) {
  alarm_ = alarm;
  alarm_.id = absl::StrFormat("%s/%d", alarm.name, alarm.type);
  capcom.SendAlarm(alarm_);
  alarm_raised_ = true;
  alarm_count_++;
}

void Process::ClearAlarm(Capcom &capcom) {
  if (!alarm_raised_) {
    return;
  }
  alarm_.status = Alarm::Status::kCleared;
  capcom.SendAlarm(alarm_);
  alarm_raised_ = false;
}

absl::Status
Process::SendTelemetryCommand(std::shared_ptr<Subsystem> subsystem,
                              const adastra::proto::telemetry::Command &cmd,
                              co::Coroutine *c) {
  Umbilical *umbilical = subsystem->FindUmbilical(compute_);
  if (umbilical == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No umbilical found for compute %s", compute_));
  }
  if (!umbilical->GetClient()->IsConnected()) {
    return absl::InternalError(
        absl::StrFormat("Umbilical %s is not connected", compute_));
  }
  return umbilical->GetClient()->SendTelemetryCommand(process_id_, cmd, c);
}

StaticProcess::StaticProcess(
    Capcom &capcom, std::string name, const std::string &compute,
    std::string executable, const stagezero::config::ProcessOptions &options,
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams)
    : Process(capcom, std::move(name), std::move(compute)),
      executable_(std::move(executable)) {
  ParseOptions(options);
  ParseStreams(streams);
}

absl::Status StaticProcess::Launch(Subsystem *subsystem, co::Coroutine *c) {
  stagezero::ProcessOptions options = {
      .description = description_,
      .args = args_,
      .startup_timeout_secs = startup_timeout_secs_,
      .telemetry_shutdown_timeout_secs = telemetry_shutdown_timeout_secs_,
      .sigint_shutdown_timeout_secs = sigint_shutdown_timeout_secs_,
      .sigterm_shutdown_timeout_secs = sigterm_shutdown_timeout_secs_,
      .notify = notify_,
      .telemetry = telemetry_,
      .interactive = interactive_,
      .interactive_terminal = subsystem->InteractiveTerminal(),
      .user = user_,
      .group = group_,
      .critical = critical_,
      .cgroup = cgroup_,
  };
  // Subsystem vars.
  for (auto &var : subsystem->Vars()) {
    options.vars.push_back(var);
  }
  // Process variables, can override subsystem vars.
  for (auto &var : vars_) {
    options.vars.push_back({var.name, var.value, var.exported});
  }

  // Local parameters.
  for (auto &param : local_parameters_.GetAllParameters()) {
    options.parameters.push_back({param.name, param.value});
  }

  options.streams = subsystem->Streams();
  for (auto &stream : streams_) {
    AddStream(options.streams, stream);
  }

  Umbilical *umbilical = subsystem->FindUmbilical(compute_);
  if (umbilical == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No umbilical found for compute %s", compute_));
  }

  absl::StatusOr<std::pair<std::string, int>> s =
      umbilical->GetClient()->LaunchStaticProcess(Name(), executable_, options,
                                                  c);
  if (!s.ok()) {
    return s.status();
  }
  process_id_ = s->first;
  pid_ = s->second;
  return absl::OkStatus();
}

absl::Status Zygote::Launch(Subsystem *subsystem, co::Coroutine *c) {
  stagezero::ProcessOptions options = {
      .description = description_,
      .args = args_,
      .startup_timeout_secs = startup_timeout_secs_,
      .telemetry_shutdown_timeout_secs = telemetry_shutdown_timeout_secs_,
      .sigint_shutdown_timeout_secs = sigint_shutdown_timeout_secs_,
      .sigterm_shutdown_timeout_secs = sigterm_shutdown_timeout_secs_,
      .notify = notify_,
      .telemetry = true,
      .user = user_,
      .group = group_,
      .critical = critical_,
      .cgroup = cgroup_,
  };
  // Subsystem vars.
  for (auto &var : subsystem->Vars()) {
    options.vars.push_back(var);
  }
  // Process variables, can override subsystem vars.
  for (auto &var : vars_) {
    options.vars.push_back({var.name, var.value, var.exported});
  }

  options.streams = subsystem->Streams();
  for (auto &stream : streams_) {
    AddStream(options.streams, stream);
  }

  Umbilical *umbilical = subsystem->FindUmbilical(compute_);
  if (umbilical == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No umbilical found for compute %s", compute_));
  }
  absl::StatusOr<std::pair<std::string, int>> s =
      umbilical->GetClient()->LaunchZygote(Name(), executable_, options, c);
  if (!s.ok()) {
    return s.status();
  }
  process_id_ = s->first;
  pid_ = s->second;
  return absl::OkStatus();
}

VirtualProcess::VirtualProcess(
    Capcom &capcom, std::string name, const std::string &compute,
    std::string zygote_name, std::string dso, std::string main_func,
    const stagezero::config::ProcessOptions &options,
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams)
    : Process(capcom, std::move(name), std::move(compute)),
      zygote_name_(std::move(zygote_name)), dso_(dso), main_func_(main_func) {
  ParseOptions(options);
  ParseStreams(streams);
}

absl::Status VirtualProcess::Launch(Subsystem *subsystem, co::Coroutine *c) {
  stagezero::ProcessOptions options = {
      .description = description_,
      .args = args_,
      .startup_timeout_secs = startup_timeout_secs_,
      .telemetry_shutdown_timeout_secs = telemetry_shutdown_timeout_secs_,
      .sigint_shutdown_timeout_secs = sigint_shutdown_timeout_secs_,
      .sigterm_shutdown_timeout_secs = sigterm_shutdown_timeout_secs_,
      .notify = notify_,
      .telemetry = telemetry_,
      .interactive = interactive_,
      .interactive_terminal = subsystem->InteractiveTerminal(),
      .user = user_,
      .group = group_,
      .cgroup = cgroup_,
  };

  // Subsystem vars.
  for (auto &var : subsystem->Vars()) {
    options.vars.push_back(var);
  }
  // Process variables, can override subsystem vars.
  for (auto &var : vars_) {
    options.vars.push_back({var.name, var.value, var.exported});
  }

  options.streams = subsystem->Streams();
  for (auto &stream : streams_) {
    AddStream(options.streams, stream);
  }

  // Local parameters.
  for (auto &param : local_parameters_.GetAllParameters()) {
    options.parameters.push_back({param.name, param.value});
  }

  Umbilical *umbilical = subsystem->FindUmbilical(compute_);
  if (umbilical == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No umbilical found for compute %s", compute_));
  }
  absl::StatusOr<std::pair<std::string, int>> s =
      umbilical->GetClient()->LaunchVirtualProcess(Name(), zygote_name_, dso_,
                                                   main_func_, options, c);
  if (!s.ok()) {
    return s.status();
  }
  process_id_ = s->first;
  pid_ = s->second;
  return absl::OkStatus();
}

} // namespace adastra::capcom
