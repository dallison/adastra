// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/client/client.h"
#include "absl/strings/str_format.h"
#include "toolbelt/hexdump.h"

namespace adastra::stagezero {

absl::Status Client::Init(toolbelt::InetAddress addr, const std::string &name,
                          int event_mask, const std::string &compute,
                          co::Coroutine *co) {
  compute_ = compute;
  auto fill_init = [name, compute, event_mask](control::Request &req) {
    auto init = req.mutable_init();
    init->set_client_name(name);
    init->set_compute(compute);
    init->set_event_mask(event_mask);
  };

  auto parse_init = [](const control::Response &resp) -> absl::StatusOr<int> {
    auto init_resp = resp.init();
    if (!init_resp.error().empty()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to initialize client connection: %s", init_resp.error()));
    }
    return init_resp.event_port();
  };

  return TCPClient::Init(addr, name, fill_init, parse_init, co);
}

absl::StatusOr<std::pair<std::string, int>> Client::LaunchStaticProcessInternal(
    const std::string &name, const std::string &executable, ProcessOptions opts,
    bool zygote, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto launch = zygote ? req.mutable_launch_zygote()
                       : req.mutable_launch_static_process();
  auto proc = launch->mutable_proc();
  proc->set_executable(executable);
  BuildProcessOptions(name, launch->mutable_opts(), opts);

  for (auto &stream : opts.streams) {
    auto *s = launch->add_streams();
    stream.ToProto(s);
  }

  adastra::stagezero::control::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, co);
  if (!status.ok()) {
    return status;
  }

  auto &launch_resp = resp.launch();
  if (!launch_resp.error().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to launch static process: %s", launch_resp.error()));
  }
  return std::make_pair(launch_resp.process_id(), launch_resp.pid());
}

absl::StatusOr<std::pair<std::string, int>> Client::LaunchVirtualProcess(
    const std::string &name, const std::string &zygote, const std::string &dso,
    const std::string &main_func, ProcessOptions opts, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto launch = req.mutable_launch_virtual_process();
  auto proc = launch->mutable_proc();
  proc->set_zygote(zygote);
  proc->set_dso(dso);
  proc->set_main_func(main_func);
  BuildProcessOptions(name, launch->mutable_opts(), opts);

  for (auto &stream : opts.streams) {
    auto *s = launch->add_streams();
    stream.ToProto(s);
  }
  adastra::stagezero::control::Response resp;
  absl::Status status = SendRequestReceiveResponse(req, resp, co);
  if (!status.ok()) {
    return status;
  }

  auto &launch_resp = resp.launch();
  if (!launch_resp.error().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to launch virtual process: %s", launch_resp.error()));
  }
  return std::make_pair(launch_resp.process_id(), launch_resp.pid());
}

void Client::BuildProcessOptions(
    const std::string &name,
    adastra::stagezero::config::ProcessOptions *options,
    ProcessOptions opts) const {
  options->set_name(name);
  options->set_description(opts.description);
  for (auto &var : opts.vars) {
    auto svar = options->add_vars();
    svar->set_name(var.name);
    svar->set_value(var.value);
    svar->set_exported(var.exported);
  }
  for (auto &param : opts.parameters) {
    auto sparam = options->add_parameters();
    sparam->set_name(param.name);
    param.value.ToProto(sparam->mutable_value());
  }
  for (auto &arg : opts.args) {
    *options->add_args() = arg;
  }
  options->set_startup_timeout_secs(opts.startup_timeout_secs);
  options->set_telemetry_shutdown_timeout_secs(
      opts.telemetry_shutdown_timeout_secs);
  options->set_sigint_shutdown_timeout_secs(opts.sigint_shutdown_timeout_secs);
  options->set_sigterm_shutdown_timeout_secs(
      opts.sigterm_shutdown_timeout_secs);
  options->set_notify(opts.notify);
  options->set_telemetry(opts.telemetry);
  options->set_interactive(opts.interactive);
  if (opts.interactive_terminal.IsPresent()) {
    opts.interactive_terminal.ToProto(options->mutable_interactive_terminal());
  }
  options->set_group(opts.group);
  options->set_user(opts.user);
  options->set_critical(opts.critical);
  options->set_cgroup(opts.cgroup);
  options->set_detached(opts.detached);
  if (opts.ns.has_value()) {
    auto *n = options->mutable_ns();
    opts.ns->ToProto(n);
  }
}

absl::Status Client::StopProcess(const std::string &process_id,
                                 co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  req.mutable_stop()->set_process_id(process_id);
  adastra::stagezero::control::Response resp;
  return SendRequestReceiveResponse(req, resp, co);
}

absl::Status Client::SendInput(const std::string &process_id, int fd,
                               const std::string &data, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto input = req.mutable_input_data();
  input->set_process_id(process_id);
  input->set_fd(fd);
  input->set_data(data);

  adastra::stagezero::control::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &input_resp = resp.input_data();
  if (!input_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to send input: %s", input_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::CloseProcessFileDescriptor(const std::string &process_id,
                                                int fd, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto close = req.mutable_close_process_file_descriptor();
  close->set_process_id(process_id);
  close->set_fd(fd);

  adastra::stagezero::control::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &close_resp = resp.close_process_file_descriptor();
  if (!close_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to close fd %d on process %s: %s", fd,
                        process_id, close_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::SetGlobalVariable(std::string name, std::string value,
                                       bool exported, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto var = req.mutable_set_global_variable();
  var->set_name(name);
  var->set_value(value);
  var->set_exported(exported);

  adastra::stagezero::control::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &var_resp = resp.set_global_variable();
  if (!var_resp.error().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to set global variable %s: %s", name, var_resp.error()));
  }
  return absl::OkStatus();
}

absl::StatusOr<std::pair<std::string, bool>>
Client::GetGlobalVariable(std::string name, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto var = req.mutable_get_global_variable();
  var->set_name(name);

  adastra::stagezero::control::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &var_resp = resp.get_global_variable();
  if (!var_resp.error().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to get global variable %s: %s", name, var_resp.error()));
  }
  return std::make_pair(var_resp.value(), var_resp.exported());
}

absl::Status Client::Abort(const std::string &reason, bool emergency,
                           co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto abort = req.mutable_abort();
  abort->set_reason(reason);
  abort->set_emergency(emergency);

  adastra::stagezero::control::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
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

absl::Status Client::RegisterCgroup(const Cgroup &cgroup, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto add = req.mutable_add_cgroup();
  cgroup.ToProto(add->mutable_cgroup());

  adastra::stagezero::control::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &add_resp = resp.add_cgroup();
  if (!add_resp.error().empty()) {
    return absl::InternalError(
        absl::StrFormat("Failed to add cgroup: %s", add_resp.error()));
  }
  return absl::OkStatus();
}

absl::Status Client::FreezeCgroup(const std::string &cgroup,
                                  co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto f = req.mutable_freeze_cgroup();
  f->set_cgroup(cgroup);

  adastra::stagezero::control::Response resp;
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

absl::Status Client::ThawCgroup(const std::string &cgroup, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto f = req.mutable_thaw_cgroup();
  f->set_cgroup(cgroup);

  adastra::stagezero::control::Response resp;
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

absl::Status Client::KillCgroup(const std::string &cgroup, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto f = req.mutable_kill_cgroup();
  f->set_cgroup(cgroup);

  adastra::stagezero::control::Response resp;
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
absl::Status Client::SetParameter(const std::string &name, parameters::Value &v,
                                  co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto x = req.mutable_set_parameter();
  x->set_name(name);
  v.ToProto(x->mutable_value());

  adastra::stagezero::control::Response resp;
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
  adastra::stagezero::control::Request req;
  auto x = req.mutable_delete_parameter();
  x->set_name(name);

  adastra::stagezero::control::Response resp;
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
  adastra::stagezero::control::Request req;
  auto x = req.mutable_upload_parameters();
  for (auto &p : params) {
    auto *param = x->add_parameters();
    param->set_name(p.name);
    p.value.ToProto(param->mutable_value());
  }
  adastra::stagezero::control::Response resp;
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

absl::Status
Client::SendTelemetryCommand(const std::string &process_id,
                             const ::stagezero::TelemetryCommand &cmd,
                             co::Coroutine *co) {
  adastra::proto::telemetry::Command tc;
  cmd.ToProto(tc);
  return SendTelemetryCommand(process_id, tc, co);
}

absl::Status
Client::SendTelemetryCommand(const std::string &process_id,
                             const adastra::proto::telemetry::Command &cmd,
                             co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  adastra::stagezero::control::Request req;
  auto x = req.mutable_send_telemetry_command();
  x->set_process_id(process_id);
  *x->mutable_command() = cmd;

  adastra::stagezero::control::Response resp;
  if (absl::Status status = SendRequestReceiveResponse(req, resp, co);
      !status.ok()) {
    return status;
  }
  auto &tele_resp = resp.send_telemetry_command();
  if (!tele_resp.error().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Failed to send telemetry command %s", tele_resp.error()));
  }
  return absl::OkStatus();
}
} // namespace adastra::stagezero
