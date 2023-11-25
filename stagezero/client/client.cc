// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/client/client.h"
#include "absl/strings/str_format.h"
#include "toolbelt/hexdump.h"

namespace stagezero {

absl::Status Client::Init(toolbelt::InetAddress addr, const std::string &name,
                          const std::string &compute, co::Coroutine *co) {

  auto fill_init = [name, compute](control::Request &req) {
    auto init = req.mutable_init();
    init->set_client_name(name);
    init->set_compute(compute);
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
  stagezero::control::Request req;
  auto launch = zygote ? req.mutable_launch_zygote()
                       : req.mutable_launch_static_process();
  auto proc = launch->mutable_proc();
  proc->set_executable(executable);
  BuildProcessOptions(name, launch->mutable_opts(), opts);

  for (auto &stream : opts.streams) {
    auto *s = launch->add_streams();
    stream.ToProto(s);
  }

  stagezero::control::Response resp;
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
  stagezero::control::Request req;
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
  stagezero::control::Response resp;
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

void Client::BuildProcessOptions(const std::string &name,
                                 stagezero::config::ProcessOptions *options,
                                 ProcessOptions opts) const {
  options->set_name(name);
  options->set_description(opts.description);
  for (auto &var : opts.vars) {
    auto svar = options->add_vars();
    svar->set_name(var.name);
    svar->set_value(var.value);
    svar->set_exported(var.exported);
  }
  for (auto &arg : opts.args) {
    *options->add_args() = arg;
  }
  options->set_startup_timeout_secs(opts.startup_timeout_secs);
  options->set_sigint_shutdown_timeout_secs(opts.sigint_shutdown_timeout_secs);
  options->set_sigterm_shutdown_timeout_secs(
      opts.sigterm_shutdown_timeout_secs);
  options->set_notify(opts.notify);
}

absl::Status Client::StopProcess(const std::string &process_id,
                                 co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  stagezero::control::Request req;
  req.mutable_stop()->set_process_id(process_id);
  stagezero::control::Response resp;
  return SendRequestReceiveResponse(req, resp, co);
}

absl::Status Client::SendInput(const std::string &process_id, int fd,
                               const std::string &data, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  stagezero::control::Request req;
  auto input = req.mutable_input_data();
  input->set_process_id(process_id);
  input->set_fd(fd);
  input->set_data(data);

  stagezero::control::Response resp;
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
  stagezero::control::Request req;
  auto close = req.mutable_close_process_file_descriptor();
  close->set_process_id(process_id);
  close->set_fd(fd);

  stagezero::control::Response resp;
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
  stagezero::control::Request req;
  auto var = req.mutable_set_global_variable();
  var->set_name(name);
  var->set_value(value);
  var->set_exported(exported);

  stagezero::control::Response resp;
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
  stagezero::control::Request req;
  auto var = req.mutable_get_global_variable();
  var->set_name(name);

  stagezero::control::Response resp;
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

absl::Status Client::Abort(const std::string &reason, co::Coroutine *co) {
  if (co == nullptr) {
    co = co_;
  }
  stagezero::control::Request req;
  req.mutable_abort()->set_reason(reason);

  stagezero::control::Response resp;
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
} // namespace stagezero