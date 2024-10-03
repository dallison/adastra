// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.
#include "capcom/client/client.h"
#include "toolbelt/hexdump.h"

namespace adastra::capcom::client {

absl::Status Client::Init(toolbelt::InetAddress addr, const std::string &name,
                          int event_mask, co::Coroutine *co) {
  auto fill_init = [name, event_mask](capcom::proto::Request &req) {
    auto init = req.mutable_init();
    init->set_client_name(name);
    init->set_event_mask(event_mask);
  };

  auto parse_init =
      [](const capcom::proto::Response &resp) -> absl::StatusOr<int> {
    auto init_resp = resp.init();
    if (!init_resp.error().empty()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to initialize client connection: %s", init_resp.error()));
    }
    return init_resp.event_port();
  };

  return TCPClient::Init(addr, name, fill_init, parse_init, co);
}

absl::StatusOr<std::shared_ptr<Event>> Client::ReadEvent(co::Coroutine *c) {
  absl::StatusOr<std::shared_ptr<adastra::proto::Event>> proto_event =
      ReadProtoEvent(c);
  if (!proto_event.ok()) {
    return proto_event.status();
  }
  auto result = std::make_shared<Event>();
  if (absl::Status status = result->FromProto(**proto_event); !status.ok()) {
    return status;
  }
  return result;
}

absl::Status Client::AddCompute(const std::string &name,
                                const toolbelt::InetAddress &addr,
                                const std::vector<Cgroup> &cgroups,
                                co::Coroutine *c) {
  if (!addr.Valid()) {
    return absl::InternalError("Invalid address");
  }
  adastra::capcom::proto::Request req;
  auto add = req.mutable_add_compute();
  auto compute = add->mutable_compute();
  compute->set_name(name);
  in_addr ip_addr = addr.IpAddress();
  compute->set_ip_addr(&ip_addr, sizeof(ip_addr));
  compute->set_port(addr.Port());

  for (auto &cgroup : cgroups) {
    auto *cg = compute->add_cgroups();
    cgroup.ToProto(cg);
  }

  adastra::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &add_resp = resp.add_compute();
  if (!add_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to add compute: %s", add_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::RemoveCompute(const std::string &name, co::Coroutine *c) {
  adastra::capcom::proto::Request req;
  auto r = req.mutable_remove_compute();
  r->set_name(name);

  adastra::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &rem_resp = resp.remove_compute();
  if (!rem_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to remove compute: %s", rem_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::AddSubsystem(const std::string &name,
                                  const SubsystemOptions &options,
                                  co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  auto add = req.mutable_add_subsystem();
  add->set_name(name);

  // Add all static processes to the proto message.
  for (auto &sproc : options.static_processes) {
    auto *proc = add->add_processes();
    auto *opts = proc->mutable_options();
    opts->set_name(sproc.name);
    opts->set_description(sproc.description);
    opts->set_sigint_shutdown_timeout_secs(sproc.sigint_shutdown_timeout_secs);
    opts->set_sigterm_shutdown_timeout_secs(
        sproc.sigterm_shutdown_timeout_secs);
    opts->set_notify(sproc.notify);
    opts->set_user(sproc.user);
    opts->set_group(sproc.group);
    opts->set_interactive(sproc.interactive);
    opts->set_critical(options.critical);
    opts->set_oneshot(sproc.oneshot);
    opts->set_cgroup(sproc.cgroup);

    auto *s = proc->mutable_static_process();
    s->set_executable(sproc.executable);
    proc->set_compute(sproc.compute);
    proc->set_max_restarts(sproc.max_restarts);
    for (auto &arg : sproc.args) {
      auto *a = opts->add_args();
      *a = arg;
    }
    for (auto &var : sproc.vars) {
      auto *v = opts->add_vars();
      v->set_name(var.name);
      v->set_value(var.value);
      v->set_exported(var.exported);
    }
    for (auto &stream : sproc.streams) {
      auto *s = proc->add_streams();
      stream.ToProto(s);
    }
  }

  // Add Zygotes.
  for (auto &z : options.zygotes) {
    auto *proc = add->add_processes();
    auto *opts = proc->mutable_options();
    opts->set_name(z.name);
    opts->set_description(z.description);
    opts->set_sigint_shutdown_timeout_secs(z.sigint_shutdown_timeout_secs);
    opts->set_sigterm_shutdown_timeout_secs(z.sigterm_shutdown_timeout_secs);
    opts->set_notify(true);
    opts->set_user(z.user);
    opts->set_group(z.group);
    opts->set_critical(options.critical);
    opts->set_cgroup(z.cgroup);

    auto *s = proc->mutable_zygote();
    s->set_executable(z.executable);
    proc->set_compute(z.compute);
    proc->set_max_restarts(z.max_restarts);
    for (auto &arg : z.args) {
      auto *a = opts->add_args();
      *a = arg;
    }
    for (auto &var : z.vars) {
      auto *v = opts->add_vars();
      v->set_name(var.name);
      v->set_value(var.value);
      v->set_exported(var.exported);
    }
    for (auto &stream : z.streams) {
      auto *s = proc->add_streams();
      stream.ToProto(s);
    }
  }

  // Add all virtual processes to the proto message.
  for (auto &vproc : options.virtual_processes) {
    auto *proc = add->add_processes();
    auto *opts = proc->mutable_options();
    opts->set_name(vproc.name);
    opts->set_description(vproc.description);
    opts->set_sigint_shutdown_timeout_secs(vproc.sigint_shutdown_timeout_secs);
    opts->set_sigterm_shutdown_timeout_secs(
        vproc.sigterm_shutdown_timeout_secs);
    opts->set_notify(vproc.notify);
    opts->set_user(vproc.user);
    opts->set_group(vproc.group);
    opts->set_critical(options.critical);
    opts->set_cgroup(vproc.cgroup);

    auto *s = proc->mutable_virtual_process();
    s->set_zygote(vproc.zygote);
    s->set_dso(vproc.dso);
    s->set_main_func(vproc.main_func);
    proc->set_compute(vproc.compute);
    proc->set_max_restarts(vproc.max_restarts);
    for (auto &arg : vproc.args) {
      auto *a = opts->add_args();
      *a = arg;
    }
    for (auto &var : vproc.vars) {
      auto *v = opts->add_vars();
      v->set_name(var.name);
      v->set_value(var.value);
      v->set_exported(var.exported);
    }
    for (auto &stream : vproc.streams) {
      auto *s = proc->add_streams();
      stream.ToProto(s);
    }
  }

  // Variables.
  for (auto &var : options.vars) {
    auto *v = add->add_vars();
    v->set_name(var.name);
    v->set_value(var.value);
    v->set_exported(var.exported);
  }

  // Streams.
  for (auto &stream : options.streams) {
    auto *s = add->add_streams();
    stream.ToProto(s);
  }

  // Args.
  for (auto &arg : options.args) {
    auto *a = add->add_args();
    *a = arg;
  }

  // Children.
  for (auto &child : options.children) {
    auto *ch = add->add_children();
    *ch = child;
  }

  add->set_max_restarts(options.max_restarts);
  add->set_critical(options.critical);
  switch (options.restart_policy) {
  case RestartPolicy::kAutomatic:
    add->set_restart_policy(
        adastra::capcom::proto::AddSubsystemRequest::AUTOMATIC);
    break;
  case RestartPolicy::kManual:
    add->set_restart_policy(
        adastra::capcom::proto::AddSubsystemRequest::MANUAL);
    break;
  case RestartPolicy::kProcessOnly:
    add->set_restart_policy(
        adastra::capcom::proto::AddSubsystemRequest::PROCESS_ONLY);
    break;
  }
  adastra::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &add_resp = resp.add_subsystem();
  if (!add_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to add subsystem: %s", add_resp.error()));
  }

  if (mode_ == ClientMode::kBlocking) {
    return WaitForSubsystemState(name, AdminState::kOffline,
                                 OperState::kOffline, c);
  }
  return absl::OkStatus();
}

absl::Status Client::RemoveSubsystem(const std::string &name, bool recursive,
                                     co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  auto r = req.mutable_remove_subsystem();
  r->set_subsystem(name);
  r->set_recursive(recursive);

  adastra::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &remove_resp = resp.remove_subsystem();
  if (!remove_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to remove subsystem: %s", remove_resp.error()));
  }

  return absl::OkStatus();
}

absl::Status Client::StartSubsystem(const std::string &name, RunMode mode,
                                    Terminal *terminal, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  auto s = req.mutable_start_subsystem();
  s->set_subsystem(name);
  s->set_interactive(mode == RunMode::kInteractive);
  if (terminal != nullptr) {
    auto *t = s->mutable_terminal();
    terminal->ToProto(t);
  }
  adastra::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &start_resp = resp.start_subsystem();
  if (!start_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to start subsystem: %s", start_resp.error()));
  }

  if (mode_ == ClientMode::kBlocking) {
    return WaitForSubsystemState(name, AdminState::kOnline, OperState::kOnline,
                                 c);
  }
  return absl::OkStatus();
}

absl::Status Client::StopSubsystem(const std::string &name, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  auto s = req.mutable_stop_subsystem();
  s->set_subsystem(name);

  adastra::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &stop_resp = resp.stop_subsystem();
  if (!stop_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to start subsystem: %s", stop_resp.error()));
  }

  if (mode_ == ClientMode::kBlocking) {
    return WaitForSubsystemState(name, AdminState::kOffline,
                                 OperState::kOffline, c);
  }
  return absl::OkStatus();
}

absl::Status Client::RestartSubsystem(const std::string &name,
                                      co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  auto s = req.mutable_restart_subsystem();
  s->set_subsystem(name);

  adastra::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &restart_resp = resp.stop_subsystem();
  if (!restart_resp.error().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to restart subsystem: %s", restart_resp.error()));
  }

  if (mode_ == ClientMode::kBlocking) {
    return WaitForSubsystemState(name, AdminState::kOnline, OperState::kOnline,
                                 c);
  }
  return absl::OkStatus();
}

absl::Status Client::RestartProcesses(const std::string &subsystem,
                                      const std::vector<std::string> &processes,
                                      co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  auto restart = req.mutable_restart_processes();
  restart->set_subsystem(subsystem);
  for (auto &process : processes) {
    restart->add_processes(process);
  }
  adastra::capcom::proto::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return status;
  }

  auto &restart_resp = resp.restart_processes();
  if (!restart_resp.error().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to restart processes: %s", restart_resp.error()));
  }

  if (mode_ == ClientMode::kBlocking) {
    return WaitForSubsystemState(subsystem, AdminState::kOnline,
                                 OperState::kOnline, c);
  }
  return absl::OkStatus();
}

absl::Status Client::WaitForSubsystemState(const std::string &subsystem,
                                           AdminState admin_state,
                                           OperState oper_state,
                                           co::Coroutine *c) {
  for (;;) {
    absl::StatusOr<std::shared_ptr<Event>> e = WaitForEvent(c);
    if (!e.ok()) {
      return e.status();
    }
    std::shared_ptr<Event> event = *e;
    if (event->type == EventType::kSubsystemStatus) {
      SubsystemStatus &s = std::get<0>(event->event);
      if (s.subsystem == subsystem) {
        if (admin_state == s.admin_state) {
          if (s.admin_state == AdminState::kOnline &&
              s.oper_state == OperState::kBroken) {
            return absl::InternalError(
                absl::StrFormat("Subsystem %s is broken", subsystem));
          }
          if (oper_state == s.oper_state) {
            return absl::OkStatus();
          }
        }
      }
    }
  }
}

absl::Status Client::Abort(const std::string &reason, bool emergency,
                           co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  auto abort = req.mutable_abort();
  abort->set_reason(reason);
  abort->set_emergency(emergency);

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &abort_resp = resp.abort();
  if (!abort_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to abort: %s", abort_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::AddGlobalVariable(const Variable &var, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  auto *v = req.mutable_add_global_variable()->mutable_var();
  v->set_name(var.name);
  v->set_value(var.value);
  v->set_exported(var.exported);

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &var_resp = resp.add_global_variable();
  if (!var_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to add global variable: %s", var_resp.error()));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::vector<SubsystemStatus>>
Client::GetSubsystems(co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  (void)req.mutable_get_subsystems();

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &get_resp = resp.get_subsystems();
  if (!get_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to get subsystems: %s", get_resp.error()));
  }

  std::vector<SubsystemStatus> result(get_resp.subsystems_size());
  int index = 0;
  for (auto &status : get_resp.subsystems()) {
    if (absl::Status s = result[index].FromProto(status); !s.ok()) {
      return s;
    }
    index++;
  }
  return result;
}

absl::StatusOr<std::vector<Alarm>> Client::GetAlarms(co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  (void)req.mutable_get_alarms();

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &get_resp = resp.get_alarms();
  if (!get_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to get alarms: %s", get_resp.error()));
  }

  std::vector<Alarm> result(get_resp.alarms_size());
  int index = 0;
  for (auto &alarm : get_resp.alarms()) {
    result[index].FromProto(alarm);
    index++;
  }
  return result;
}

absl::Status Client::SendInput(const std::string &subsystem,
                               const std::string &process, int fd,
                               const std::string &data, co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  auto input = req.mutable_input();
  input->set_subsystem(subsystem);
  input->set_process(process);
  input->set_fd(fd);
  input->set_data(data);

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &input_resp = resp.input();
  if (!input_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to send input: %s", input_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::CloseFd(const std::string &subsystem,
                             const std::string &process, int fd,
                             co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  adastra::capcom::proto::Request req;
  auto close = req.mutable_close_fd();
  close->set_subsystem(subsystem);
  close->set_process(process);
  close->set_fd(fd);

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, c);
      !status.ok()) {
    return status;
  }
  auto &close_resp = resp.close_fd();
  if (!close_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to close fd: %s", close_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::FreezeCgroup(const std::string &compute,
                                  const std::string &cgroup,
                                  co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::capcom::proto::Request req;
  auto f = req.mutable_freeze_cgroup();
  f->set_compute(compute);
  f->set_cgroup(cgroup);

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &freeze_resp = resp.freeze_cgroup();
  if (!freeze_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to freeze cgroup: %s", freeze_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::ThawCgroup(const std::string &compute,
                                const std::string &cgroup, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::capcom::proto::Request req;
  auto f = req.mutable_thaw_cgroup();
  f->set_compute(compute);
  f->set_cgroup(cgroup);

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &thaw_resp = resp.thaw_cgroup();
  if (!thaw_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to thaw cgroup: %s", thaw_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::KillCgroup(const std::string &compute,
                                const std::string &cgroup, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::capcom::proto::Request req;
  auto f = req.mutable_kill_cgroup();
  f->set_compute(compute);
  f->set_cgroup(cgroup);

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &kill_resp = resp.kill_cgroup();
  if (!kill_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to kill cgroup: %s", kill_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::SetParameter(const std::string& name, const parameters::Value &v,
                                  co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::capcom::proto::Request req;
  auto x = req.mutable_set_parameter();
  x->set_name(name);
  v.ToProto(x->mutable_value());

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &kill_resp = resp.set_parameter();
  if (!kill_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to set parameter: %s", kill_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::DeleteParameter(const std::string &name,
                                     co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::capcom::proto::Request req;
  auto x = req.mutable_delete_parameter();
  x->set_name(name);

  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &kill_resp = resp.delete_parameter();
  if (!kill_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to delete parameter: %s", kill_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status
Client::UploadParameters(const std::vector<parameters::Parameter> &params,
                         co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::capcom::proto::Request req;
  auto x = req.mutable_upload_parameters();
  for (auto &p : params) {
    auto *param = x->add_parameters();
    param->set_name(p.GetName());
    p.GetValue().ToProto(param->mutable_value());
  }
  adastra::capcom::proto::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &kill_resp = resp.upload_parameters();
  if (!kill_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to upload parameter: %s", kill_resp.error()));
  }
  return absl::OkStatus();
}
} // namespace adastra::capcom::client
