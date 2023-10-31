#include "capcom/client_handler.h"
#include "absl/strings/str_format.h"
#include "capcom/capcom.h"
#include "toolbelt/hexdump.h"

#include <iostream>

namespace stagezero::capcom {

ClientHandler::~ClientHandler() {}


  absl::Status ClientHandler::SendSubsystemStatusEvent(std::shared_ptr<Subsystem> subsystem) {
  auto event = std::make_unique<capcom::proto::Event>();
  auto s = event->mutable_subsystem_status();
  subsystem->BuildStatusEvent(s);
  return QueueEvent(std::move(event));
  }

absl::Status
ClientHandler::HandleMessage(const stagezero::capcom::proto::Request &req,
                             stagezero::capcom::proto::Response &resp,
                             co::Coroutine *c) {
  std::cerr << "capcom handling message " << req.DebugString() << std::endl;
  switch (req.request_case()) {
  case stagezero::capcom::proto::Request::kInit:
    HandleInit(req.init(), resp.mutable_init(), c);
    break;
  case stagezero::capcom::proto::Request::kAddSubsystem:
    HandleAddSubsystem(req.add_subsystem(), resp.mutable_add_subsystem(), c);
    break;

  case stagezero::capcom::proto::Request::kRemoveSubsystem:
    HandleRemoveSubsystem(req.remove_subsystem(),
                          resp.mutable_remove_subsystem(), c);
    break;
  case stagezero::capcom::proto::Request::kStartSubsystem:
    HandleStartSubsystem(req.start_subsystem(), resp.mutable_start_subsystem(),
                         c);
    break;
  case stagezero::capcom::proto::Request::kStopSubsystem:
    HandleStopSubsystem(req.stop_subsystem(), resp.mutable_stop_subsystem(), c);
    break;

  case stagezero::capcom::proto::Request::REQUEST_NOT_SET:
    return absl::InternalError("Protocol error: unknown request");
  }
  return absl::OkStatus();
}

void ClientHandler::HandleInit(const stagezero::capcom::proto::InitRequest &req,
                               stagezero::capcom::proto::InitResponse *response,
                               co::Coroutine *c) {
   absl::StatusOr<int> s = Init(req.client_name(), c);
  if (!s.ok()) {
    response->set_error(s.status().ToString());
    return;
  }
  response->set_event_port(*s);
}

void ClientHandler::AddCoroutine(std::unique_ptr<co::Coroutine> c) {
  capcom_.AddCoroutine(std::move(c));
}

void ClientHandler::HandleAddSubsystem(
    const stagezero::capcom::proto::AddSubsystemRequest &req,
    stagezero::capcom::proto::AddSubsystemResponse *response,
    co::Coroutine *c) {
  std::cerr << "HandleAddSubsystem" << std::endl;
  auto subsystem = std::make_shared<Subsystem>(req.name(), capcom_);
  if (!capcom_.AddSubsystem(req.name(), subsystem)) {
    response->set_error(absl::StrFormat("Failed to add subsystem %s; already exists", req.name()));
    return;
  }

  // Add the processes to the subsystem.
  for (auto &proc : req.processes()) {
    switch (proc.proc_case()) {
    case stagezero::capcom::proto::Process::kStaticProcess:
      if (absl::Status status = subsystem->AddStaticProcess(
              proc.static_process(), proc.options());
          !status.ok()) {
        response->set_error(
            absl::StrFormat("Failed to add static process %s: %s",
                            proc.options().name(), status.ToString()));
        return;
      }
      break;
    case stagezero::capcom::proto::Process::kZygote:
      break;
    case stagezero::capcom::proto::Process::kVirtualProcess:
      break;
    case stagezero::capcom::proto::Process::PROC_NOT_SET:
      break;
    }
  }

  // Start the subsystem running.  This spawns a coroutine and returns without
  // bloocking.
  subsystem->Run();
}

void ClientHandler::HandleRemoveSubsystem(
    const stagezero::capcom::proto::RemoveSubsystemRequest &req,
    stagezero::capcom::proto::RemoveSubsystemResponse *response,
    co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  if (absl::Status status = subsystem->Remove(req.recursive()); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to remove subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
  }
}

void ClientHandler::HandleStartSubsystem(
    const stagezero::capcom::proto::StartSubsystemRequest &req,
    stagezero::capcom::proto::StartSubsystemResponse *response,
    co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  Message message = {.code = Message::kChangeAdmin, .state.admin = AdminState::kOnline};
  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to start subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
    return;
  }
}

void ClientHandler::HandleStopSubsystem(
    const stagezero::capcom::proto::StopSubsystemRequest &req,
    stagezero::capcom::proto::StopSubsystemResponse *response,
    co::Coroutine *c) {
  std::shared_ptr<Subsystem> subsystem = capcom_.FindSubsystem(req.subsystem());
  if (subsystem == nullptr) {
    response->set_error(
        absl::StrFormat("No such subsystem %s", req.subsystem()));
    return;
  }
  Message message = {.code = Message::kChangeAdmin, .state.admin = AdminState::kOffline};
  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to stop subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
    return;
  }
}
} // namespace stagezero::capcom
