// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/client_handler.h"
#include "absl/strings/str_format.h"
#include "stagezero/cgroup.h"
#include "stagezero/stagezero.h"
#include "toolbelt/clock.h"
#include "toolbelt/hexdump.h"

#include <iostream>

namespace adastra::stagezero {

SymbolTable *ClientHandler::GetGlobalSymbols() const {
  return &stagezero_.global_symbols_;
}

toolbelt::Logger &ClientHandler::GetLogger() const {
  return stagezero_.logger_;
}

co::CoroutineScheduler &ClientHandler::GetScheduler() const {
  return stagezero_.co_scheduler_;
}

std::shared_ptr<Zygote>
ClientHandler::FindZygote(const std::string &name) const {
  return stagezero_.FindZygote(name);
}

void ClientHandler::AddCoroutine(std::unique_ptr<co::Coroutine> c) {
  stagezero_.AddCoroutine(std::move(c));
}

const std::string &ClientHandler::GetCompute() const {
  return stagezero_.compute_;
}

void ClientHandler::StopAllCoroutines() { stagezero_.coroutines_.clear(); }

absl::Status ClientHandler::HandleMessage(const control::Request &req,
                                          control::Response &resp,
                                          co::Coroutine *c) {
  switch (req.request_case()) {
  case control::Request::kInit:
    HandleInit(req.init(), resp.mutable_init(), c);
    break;

  case control::Request::kLaunchStaticProcess:
    HandleLaunchStaticProcess(std::move(req.launch_static_process()),
                              resp.mutable_launch(), c);
    break;

  case control::Request::kLaunchZygote:
    HandleLaunchZygote(std::move(req.launch_zygote()), resp.mutable_launch(),
                       c);
    break;

  case control::Request::kLaunchVirtualProcess:
    HandleLaunchVirtualProcess(std::move(req.launch_virtual_process()),
                               resp.mutable_launch(), c);
    break;

  case control::Request::kStop:
    HandleStopProcess(req.stop(), resp.mutable_stop(), c);
    break;

  case control::Request::kInputData:
    HandleInputData(req.input_data(), resp.mutable_input_data(), c);
    break;

  case control::Request::kCloseProcessFileDescriptor:
    HandleCloseProcessFileDescriptor(
        req.close_process_file_descriptor(),
        resp.mutable_close_process_file_descriptor(), c);
    break;

  case control::Request::kSetGlobalVariable:
    HandleSetGlobalVariable(req.set_global_variable(),
                            resp.mutable_set_global_variable(), c);
    break;
  case control::Request::kGetGlobalVariable:
    HandleGetGlobalVariable(req.get_global_variable(),
                            resp.mutable_get_global_variable(), c);
    break;

  case control::Request::kAbort:
    HandleAbort(req.abort(), resp.mutable_abort(), c);
    break;

  case control::Request::kAddCgroup:
    HandleAddCgroup(req.add_cgroup(), resp.mutable_add_cgroup(), c);
    break;

  case control::Request::kRemoveCgroup:
    HandleRemoveCgroup(req.remove_cgroup(), resp.mutable_remove_cgroup(), c);
    break;

  case control::Request::kFreezeCgroup:
    HandleFreezeCgroup(req.freeze_cgroup(), resp.mutable_freeze_cgroup(), c);
    break;

  case control::Request::kThawCgroup:
    HandleThawCgroup(req.thaw_cgroup(), resp.mutable_thaw_cgroup(), c);
    break;

  case control::Request::kKillCgroup:
    HandleKillCgroup(req.kill_cgroup(), resp.mutable_kill_cgroup(), c);
    break;

  case control::Request::kSetParameter:
    HandleSetParameter(req.set_parameter(), resp.mutable_set_parameter(), c);
    break;

  case control::Request::kDeleteParameter:
    HandleDeleteParameter(req.delete_parameter(),
                          resp.mutable_delete_parameter(), c);
    break;

  case control::Request::kUploadParameters:
    HandleUploadParameters(req.upload_parameters(),
                           resp.mutable_upload_parameters(), c);
    break;

  case control::Request::REQUEST_NOT_SET:
    return absl::InternalError("Protocol error: unknown request");
  }
  return absl::OkStatus();
}

void ClientHandler::HandleInit(const control::InitRequest &req,
                               control::InitResponse *response,
                               co::Coroutine *c) {
  absl::StatusOr<int> s = Init(req.client_name(), req.event_mask(), [] {}, c);
  if (!s.ok()) {
    response->set_error(s.status().ToString());
    return;
  }
  stagezero_.compute_ = req.compute();
  response->set_event_port(*s);
  // Add a "compute" global symbol.
  stagezero_.global_symbols_.AddSymbol("compute", req.compute(), false);
}

void ClientHandler::HandleLaunchStaticProcess(
    const control::LaunchStaticProcessRequest &&req,
    control::LaunchResponse *response, co::Coroutine *c) {
  auto proc = std::make_shared<StaticProcess>(
      GetScheduler(), stagezero_, this->shared_from_this(), std::move(req));
  absl::Status status = proc->Start(c);
  if (!status.ok()) {
    response->set_error(status.ToString());
    return;
  }
  std::string process_id = proc->GetId();
  response->set_process_id(process_id);
  response->set_pid(proc->GetPid());

  if (!stagezero_.AddProcess(process_id, proc)) {
    response->set_error(
        absl::StrFormat("Unable to add process %s", process_id));
    return;
  }
  AddProcess(process_id, proc);
}

void ClientHandler::HandleLaunchZygote(
    const control::LaunchStaticProcessRequest &&req,
    control::LaunchResponse *response, co::Coroutine *c) {
  auto zygote = std::make_shared<Zygote>(GetScheduler(), stagezero_,
                                         shared_from_this(), std::move(req));
  absl::Status status = zygote->Start(c);
  if (!status.ok()) {
    response->set_error(status.ToString());
    return;
  }
  std::string process_id = zygote->GetId();
  response->set_process_id(process_id);
  response->set_pid(zygote->GetPid());
  if (!stagezero_.AddZygote(zygote->Name(), process_id, zygote)) {
    response->set_error(absl::StrFormat("Unable to add zygote %s(%s)",
                                        zygote->Name(), process_id));
    return;
  }
  AddProcess(process_id, zygote);
}

void ClientHandler::HandleLaunchVirtualProcess(
    const control::LaunchVirtualProcessRequest &&req,
    control::LaunchResponse *response, co::Coroutine *c) {
  auto proc = std::make_shared<VirtualProcess>(
      GetScheduler(), stagezero_, shared_from_this(), std::move(req));
  absl::Status status = proc->Start(c);
  if (!status.ok()) {
    response->set_error(status.ToString());
    return;
  }
  std::string process_id = proc->GetId();
  response->set_process_id(process_id);
  response->set_pid(proc->GetPid());
  if (!stagezero_.AddProcess(process_id, proc)) {
    response->set_error(
        absl::StrFormat("Unable to add process %s", process_id));
    return;
  }
  AddProcess(process_id, proc);
}

void ClientHandler::HandleStopProcess(const control::StopProcessRequest &req,
                                      control::StopProcessResponse *response,
                                      co::Coroutine *c) {
  std::shared_ptr<Process> proc = stagezero_.FindProcess(req.process_id());
  if (proc == nullptr) {
    response->set_error(
        absl::StrFormat("No such process %s", req.process_id()));
    return;
  }
  absl::Status status = proc->Stop(c);
  if (!status.ok()) {
    response->set_error(absl::StrFormat("Failed to stop process %s: %s",
                                        req.process_id(), status.ToString()));
  }
}

void ClientHandler::HandleInputData(const control::InputDataRequest &req,
                                    control::InputDataResponse *response,
                                    co::Coroutine *c) {
  std::shared_ptr<Process> proc = stagezero_.FindProcess(req.process_id());
  if (proc == nullptr) {
    response->set_error(
        absl::StrFormat("No such process %s", req.process_id()));
    return;
  }
  if (absl::Status status = proc->SendInput(req.fd(), req.data(), c);
      !status.ok()) {
    response->set_error(
        absl::StrFormat("Unable to send input data to process %s: %s",
                        req.process_id(), status.ToString()));
  }
}

void ClientHandler::HandleCloseProcessFileDescriptor(
    const control::CloseProcessFileDescriptorRequest &req,
    control::CloseProcessFileDescriptorResponse *response, co::Coroutine *c) {
  std::shared_ptr<Process> proc = stagezero_.FindProcess(req.process_id());
  if (proc == nullptr) {
    response->set_error(
        absl::StrFormat("No such process %s", req.process_id()));
    return;
  }
  if (absl::Status status = proc->CloseFileDescriptor(req.fd()); !status.ok()) {
    response->set_error(status.ToString());
  }
}

absl::Status
ClientHandler::SendProcessStartEvent(const std::string &process_id) {
  auto event = std::make_shared<control::Event>();
  auto start = event->mutable_start();
  start->set_process_id(process_id);
  return QueueEvent(std::move(event));
}

absl::Status ClientHandler::SendProcessStopEvent(const std::string &process_id,
                                                 bool exited, int exit_status,
                                                 int term_signal) {
  auto event = std::make_shared<control::Event>();
  auto stop = event->mutable_stop();
  stop->set_process_id(process_id);
  if (exited) {
    stop->set_reason(control::StopEvent::EXIT);
    stop->set_sig_or_status(exit_status);
  } else {
    stop->set_reason(control::StopEvent::SIGNAL);
    stop->set_sig_or_status(term_signal);
  }

  return QueueEvent(std::move(event));
}

absl::Status ClientHandler::SendOutputEvent(const std::string &process_id,
                                            int fd, const char *data,
                                            size_t len) {
  if ((event_mask_ & kOutputEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<control::Event>();
  auto output = event->mutable_output();
  output->set_process_id(process_id);
  output->set_data(data, len);
  output->set_fd(fd);
  return QueueEvent(std::move(event));
}

absl::Status
ClientHandler::SendParameterUpdateEvent(const std::string &name,
                                        const parameters::Value &value) {
  if ((event_mask_ & kParameterEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<control::Event>();
  auto p = event->mutable_parameter();
  auto u = p->mutable_update();
  u->set_name(name);
  value.ToProto(u->mutable_value());
  return QueueEvent(std::move(event));
}

absl::Status ClientHandler::SendParameterDeleteEvent(const std::string &name) {
  if ((event_mask_ & kParameterEvents) == 0) {
    return absl::OkStatus();
  }
  auto event = std::make_shared<control::Event>();
  auto p = event->mutable_parameter();
  p->set_delete_(name);
  return QueueEvent(std::move(event));
}

void ClientHandler::KillAllProcesses() {
  // Copy all processes out of the processes_ map as we will
  // be removing them as they are killed.
  std::vector<std::shared_ptr<Process>> procs;

  for (auto & [ id, proc ] : processes_) {
    if (!proc->IsDetached()) {
      procs.push_back(proc);
    }
  }
  for (auto &proc : procs) {
    proc->KillNow();
  }
}

absl::Status ClientHandler::RemoveProcess(Process *proc) {
  std::string id = proc->GetId();
  auto it = processes_.find(id);
  if (it == processes_.end()) {
    return absl::InternalError(absl::StrFormat("No such process %s", id));
  }
  processes_.erase(it);

  return stagezero_.RemoveProcess(proc);
}

void ClientHandler::TryRemoveProcess(std::shared_ptr<Process> proc) {
  std::string id = proc->GetId();
  auto it = processes_.find(id);
  if (it != processes_.end()) {
    processes_.erase(it);
  }

  stagezero_.TryRemoveProcess(proc);
}

void ClientHandler::HandleSetGlobalVariable(
    const control::SetGlobalVariableRequest &req,
    control::SetGlobalVariableResponse *response, co::Coroutine *c) {
  stagezero_.global_symbols_.AddSymbol(req.name(), req.value(), req.exported());
}

void ClientHandler::HandleGetGlobalVariable(
    const control::GetGlobalVariableRequest &req,
    control::GetGlobalVariableResponse *response, co::Coroutine *c) {
  Symbol *sym = stagezero_.global_symbols_.FindSymbol(req.name());
  if (sym == nullptr) {
    response->set_error(absl::StrFormat("No such variable %s", req.name()));
    return;
  }
  response->set_name(sym->Name());
  response->set_value(sym->Value());
  response->set_exported(sym->Exported());
}

void ClientHandler::HandleAbort(const control::AbortRequest &req,
                                control::AbortResponse *response,
                                co::Coroutine *c) {
  GetLogger().Log(toolbelt::LogLevel::kError, "Aborting all processes: %s",
                  req.reason().c_str());
  stagezero_.KillAllProcesses(req.emergency(), c);
  GetLogger().Log(toolbelt::LogLevel::kError, "All processes aborted: %s",
                  req.reason().c_str());
}

void ClientHandler::HandleAddCgroup(const control::AddCgroupRequest &req,
                                    control::AddCgroupResponse *response,
                                    co::Coroutine *c) {
  Cgroup cgroup;
  cgroup.FromProto(req.cgroup());

  if (!stagezero_.AddCgroup(req.cgroup().name(), cgroup)) {
    response->set_error(absl::StrFormat(
        "Failed to add cgroup %s as it already exists", req.cgroup().name()));
    return;
  }
  if (absl::Status status = stagezero_.RegisterCgroup(cgroup); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to register cgroup %s: %s",
                                        req.cgroup().name(),
                                        status.ToString()));
  }
}

void ClientHandler::HandleRemoveCgroup(const control::RemoveCgroupRequest &req,
                                       control::RemoveCgroupResponse *response,
                                       co::Coroutine *c) {
  if (absl::Status status = stagezero_.RemoveCgroup(req.cgroup());
      !status.ok()) {
    response->set_error(absl::StrFormat("Failed to remove cgroup %s: %s",
                                        req.cgroup(), status.ToString()));
    return;
  }
  if (absl::Status status = stagezero_.UnregisterCgroup(req.cgroup());
      !status.ok()) {
    response->set_error(absl::StrFormat("Failed to unregister cgroup %s: %s",
                                        req.cgroup(), status.ToString()));
  }
}

void ClientHandler::HandleFreezeCgroup(const control::FreezeCgroupRequest &req,
                                       control::FreezeCgroupResponse *response,
                                       co::Coroutine *c) {
  if (absl::Status status =
          adastra::stagezero::FreezeCgroup(req.cgroup(), GetLogger());
      !status.ok()) {
    response->set_error(absl::StrFormat("Failed to freeze cgroup %s: %s",
                                        req.cgroup(), status.ToString()));
  }
}

void ClientHandler::HandleThawCgroup(const control::ThawCgroupRequest &req,
                                     control::ThawCgroupResponse *response,
                                     co::Coroutine *c) {

  if (absl::Status status =
          adastra::stagezero::ThawCgroup(req.cgroup(), GetLogger());
      !status.ok()) {
    response->set_error(absl::StrFormat("Failed to freeze cgroup %s: %s",
                                        req.cgroup(), status.ToString()));
  }
}

void ClientHandler::HandleKillCgroup(const control::KillCgroupRequest &req,
                                     control::KillCgroupResponse *response,
                                     co::Coroutine *c) {
  if (absl::Status status =
          adastra::stagezero::KillCgroup(req.cgroup(), GetLogger());
      !status.ok()) {
    response->set_error(absl::StrFormat("Failed to freeze cgroup %s: %s",
                                        req.cgroup(), status.ToString()));
  }
}

void ClientHandler::HandleSetParameter(const control::SetParameterRequest &req,
                                       control::SetParameterResponse *response,
                                       co::Coroutine *c) {
  parameters::Value v;
  v.FromProto(req.value());
  if (absl::Status status = stagezero_.SetParameter(req.name(), v, c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleDeleteParameter(
    const control::DeleteParameterRequest &req,
    control::DeleteParameterResponse *response, co::Coroutine *c) {
  if (absl::Status status = stagezero_.DeleteParameter(req.name(), c);
      !status.ok()) {
    response->set_error(status.ToString());
  }
}

void ClientHandler::HandleUploadParameters(
    const control::UploadParametersRequest &req,
    control::UploadParametersResponse *response, co::Coroutine *c) {
  std::vector<parameters::Parameter> params;

  for (auto &p : req.parameters()) {
    parameters::Value v;
    v.FromProto(p.value());
    params.push_back({p.name(), std::move(v)});
  }

  if (absl::Status status = stagezero_.UploadParameters(params, c);
      !status.ok()) {
    response->set_error(status.ToString());
    return;
  }
}
} // namespace adastra::stagezero
