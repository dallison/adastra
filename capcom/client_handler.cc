#include "capcom/client_handler.h"
#include "absl/strings/str_format.h"
#include "capcom/capcom.h"
#include "toolbelt/hexdump.h"

#include <iostream>

namespace stagezero::capcom {

ClientHandler::~ClientHandler() {}

co::CoroutineScheduler &ClientHandler::GetScheduler() const {
  return capcom_.co_scheduler_;
}

toolbelt::Logger &ClientHandler::GetLogger() const { return capcom_.logger_; }

void ClientHandler::AddCoroutine(std::unique_ptr<co::Coroutine> c) {
  capcom_.AddCoroutine(std::move(c));
}

absl::Status ClientHandler::SendSubsystemStatusEvent(Subsystem *subsystem) {
  auto event = std::make_unique<capcom::proto::Event>();
  auto s = event->mutable_subsystem_status();
  subsystem->BuildStatusEvent(s);
  return QueueEvent(std::move(event));
}

absl::Status ClientHandler::SendAlarm(const Alarm& alarm) {
  auto event = std::make_unique<capcom::proto::Event>();
  auto a = event->mutable_alarm();
  alarm.ToProto(a);
  std::cerr << "SENDING ALARM " << a->DebugString() << std::endl;
  return QueueEvent(std::move(event));
}

absl::Status ClientHandler::HandleMessage(const proto::Request &req,
                                          proto::Response &resp,
                                          co::Coroutine *c) {
  std::cerr << "capcom handling message " << req.DebugString() << std::endl;
  switch (req.request_case()) {
  case proto::Request::kInit:
    HandleInit(req.init(), resp.mutable_init(), c);
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

  case proto::Request::REQUEST_NOT_SET:
    return absl::InternalError("Protocol error: unknown request");
  }
  return absl::OkStatus();
}

void ClientHandler::HandleInit(const proto::InitRequest &req,
                               proto::InitResponse *response,
                               co::Coroutine *c) {
  absl::StatusOr<int> s = Init(req.client_name(), c);
  if (!s.ok()) {
    response->set_error(s.status().ToString());
    return;
  }
  response->set_event_port(*s);
}

void ClientHandler::HandleAddSubsystem(const proto::AddSubsystemRequest &req,
                                       proto::AddSubsystemResponse *response,
                                       co::Coroutine *c) {
  std::cerr << "HandleAddSubsystem" << std::endl;
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

  auto subsystem = std::make_shared<Subsystem>(req.name(), capcom_);
  if (!capcom_.AddSubsystem(req.name(), subsystem)) {
    response->set_error(absl::StrFormat(
        "Failed to add subsystem %s; already exists", req.name()));
    return;
  }

  // Add the processes to the subsystem.
  for (auto &proc : req.processes()) {
    switch (proc.proc_case()) {
    case proto::Process::kStaticProcess:
      if (absl::Status status = subsystem->AddStaticProcess(
              proc.static_process(), proc.options());
          !status.ok()) {
        response->set_error(
            absl::StrFormat("Failed to add static process %s: %s",
                            proc.options().name(), status.ToString()));
        return;
      }
      break;
    case proto::Process::kZygote:
      break;
    case proto::Process::kVirtualProcess:
      break;
    case proto::Process::PROC_NOT_SET:
      break;
    }
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
                     .state.admin = AdminState::kOnline, .client_id = id_};
  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to start subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
    return;
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
                     .state.admin = AdminState::kOffline, .client_id = id_};
  if (absl::Status status = subsystem->SendMessage(message); !status.ok()) {
    response->set_error(absl::StrFormat("Failed to stop subsystem %s: %s",
                                        req.subsystem(), status.ToString()));
    return;
  }
}
} // namespace stagezero::capcom
