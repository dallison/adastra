#include "capcom/subsystem.h"
#include "capcom/capcom.h"

#include <unistd.h>

namespace stagezero::capcom {
Subsystem::Subsystem(std::string name, Capcom &capcom)
    : name_(std::move(name)), capcom_(capcom) {
  // Build the command pipe.
  if (absl::Status status = BuildMessagePipe(); !status.ok()) {
    capcom_.logger_.Log(toolbelt::LogLevel::kError, "%s",
                        status.ToString().c_str());
  }
  // Open the interrupt trigger.
  if (absl::Status status = interrupt_.Open(); !status.ok()) {
    capcom_.logger_.Log(toolbelt::LogLevel::kError,
                        "Failed to open triggerfd: %s",
                        status.ToString().c_str());
  }
}

co::CoroutineScheduler &Subsystem::Scheduler() { return capcom_.co_scheduler_; }

toolbelt::Logger &Subsystem::GetLogger() const { return capcom_.logger_; }

void Subsystem::Run() {
  // Why not std::make_unique?  We need to use the shared_from_this()
  // result in here and std::make_unique requires that everything be
  // public.
  co::Coroutine *runner = new co::Coroutine(
      capcom_.co_scheduler_, [subsys = shared_from_this()](co::Coroutine * c) {
        subsys->capcom_.logger_.Log(toolbelt::LogLevel::kInfo,
                                    "Subsystem %s is running",
                                    subsys->Name().c_str());

        // Connect to the stagezero server.
        subsys->stagezero_client_ = std::make_unique<stagezero::Client>();
        absl::Status status = subsys->stagezero_client_->Init(
            subsys->capcom_.GetStageZeroAddress(), subsys->name_, c);
        if (!status.ok()) {
          subsys->capcom_.logger_.Log(toolbelt::LogLevel::kError,
                                      "Failed to connect to stagezero: %s",
                                      status.ToString().c_str());
          return;
        }

        // Run the subsystem in offline state.
        subsys->running_ = true;
        subsys->admin_state_ = AdminState::kOffline;
        subsys->EnterState(&Subsystem::Offline, 0);

        // This coroutine now exits, but the Offline coroutine is running,
        // listening for events.
      });
  capcom_.AddCoroutine(std::unique_ptr<co::Coroutine>(runner));
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
  if (absl::Status status = capcom_.RemoveSubsystem(Name()); !status.ok()) {
    return status;
  }
  subsys->Stop();
  return absl::OkStatus();
}

absl::Status Subsystem::BuildMessagePipe() {
  int pipes[2];
  int e = ::pipe(pipes);
  if (e == -1) {
    return absl::InternalError(
        absl::StrFormat("Failed to open message pipe for subsystem %s: %s",
                        name_, strerror(errno)));
  }
  incoming_message_.SetFd(pipes[0]);
  message_.SetFd(pipes[1]);
  return absl::OkStatus();
}

absl::Status Subsystem::SendMessage(const Message &message) const {
  ssize_t n = ::write(message_.Fd(), &message, sizeof(message));
  if (n != sizeof(message)) {
    return absl::InternalError(absl::StrFormat(
        "Failed to send message to subsystem %s: %s", name_, strerror(errno)));
  }
  return absl::OkStatus();
}

void Subsystem::NotifyParents() {
  Message message = {
      .code = Message::kReportOper, .sender = this, .state.oper = oper_state_};
  for (auto &parent : parents_) {
    if (absl::Status status = parent->SendMessage(message); !status.ok()) {
      GetLogger().Log(toolbelt::LogLevel::kError,
                      "Unable to notify parent %s of oper state change for "
                      "subsystem %s: %s",
                      parent->Name().c_str(), Name().c_str(),
                      status.ToString().c_str());
    }
  }
}

void Subsystem::SendToChildren(AdminState state, uint32_t client_id) {
  Message message = {.code = Message::kChangeAdmin,
                     .sender = this,
                     .state.admin = state,
                     .client_id = client_id};
  for (auto &child : children_) {
    if (absl::Status status = child->SendMessage(message); !status.ok()) {
      GetLogger().Log(toolbelt::LogLevel::kError,
                      "Unable to send admin state to %s for "
                      "subsystem %s: %s",
                      child->Name().c_str(), Name().c_str(),
                      status.ToString().c_str());
    }
  }
}

absl::StatusOr<Message> Subsystem::ReadMessage() const {
  Message message;
  ssize_t n = ::read(incoming_message_.Fd(), &message, sizeof(message));
  if (n != sizeof(message)) {
    return absl::InternalError(absl::StrFormat(
        "Failed to read message in subsystem %s: %s", name_, strerror(errno)));
  }
  return message;
}

void Subsystem::LaunchProcesses(co::Coroutine *c) {
  for (auto &proc : processes_) {
    absl::Status status = proc->Launch(*stagezero_client_, c);
    if (!status.ok()) {
      capcom_.logger_.Log(toolbelt::LogLevel::kError,
                          "Failed to launch process %s: %s",
                          proc->Name().c_str(), status.ToString().c_str());
      continue;
    }
    RecordProcessId(proc->GetProcessId(), proc.get());
  }
}

void Subsystem::StopProcesses(co::Coroutine *c) {
  for (auto &proc : processes_) {

    absl::Status status = proc->Stop(*stagezero_client_, c);
    if (!status.ok()) {
      capcom_.logger_.Log(toolbelt::LogLevel::kError,
                          "Failed to stop process %s: %s", proc->Name().c_str(),
                          status.ToString().c_str());
      continue;
    }
  }
}

void Subsystem::EnterState(
    std::function<void(std::shared_ptr<Subsystem>, uint32_t, co::Coroutine *)>
        func,
    uint32_t client_id) {

  co::Coroutine *coroutine = new co::Coroutine(
      Scheduler(),
      [ subsystem = shared_from_this(), func = std::move(func),
        client_id ](co::Coroutine * c) { func(subsystem, client_id, c); },
      "state coroutine");

  capcom_.AddCoroutine(std::unique_ptr<co::Coroutine>(coroutine));
}

void Subsystem::ProcessEvents(
    co::Coroutine *c,
    std::function<bool(EventSource, co::Coroutine *)> handler) {
  auto subsystem = shared_from_this();

  std::vector<struct pollfd> fds;
  fds.push_back({subsystem->stagezero_client_->GetEventFd().Fd(), POLLIN});
  fds.push_back({subsystem->interrupt_.GetPollFd().Fd(), POLLIN});
  fds.push_back({subsystem->incoming_message_.Fd(), POLLIN});

  while (subsystem->running_) {
    int fd = c->Wait(fds);
    if (fd == subsystem->interrupt_.GetPollFd().Fd()) {
      // Interrupt.
      subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kInfo,
                                     "Subsystem %s interrupt",
                                     subsystem->Name().c_str());
      subsystem->interrupt_.Clear();
      continue;
    }
    EventSource event_source = EventSource::kUnknown;
    if (fd == subsystem->incoming_message_.Fd()) {
      event_source = EventSource::kMessage;
    } else if (fd == subsystem->stagezero_client_->GetEventFd().Fd()) {
      event_source = EventSource::kStageZero;
    }
    if (event_source != EventSource::kUnknown) {
      if (!handler(event_source, c)) {
        break;
      }
    } else {
      subsystem->capcom_.logger_.Log(
          toolbelt::LogLevel::kError,
          "Event from unknown source in subsystem %s",
          subsystem->Name().c_str());
    }
  }
}

void Subsystem::Offline(uint32_t client_id, co::Coroutine *c) {
  oper_state_ = OperState::kOffline;
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);
  ProcessEvents(c, [ subsystem = shared_from_this(),
                     client_id ](EventSource event_source, co::Coroutine * c) {
    if (event_source == EventSource::kMessage) {
      // Incoming message.
      absl::StatusOr<Message> message = subsystem->ReadMessage();
      if (!message.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError, "%s",
                                       message.status().ToString().c_str());
      }
      switch (message->code) {
      case Message::kChangeAdmin:
        if (message->state.admin == AdminState::kOnline) {
          subsystem->admin_state_ = AdminState::kOnline;
          subsystem->active_clients_.Set(client_id);
          subsystem->EnterState(&Subsystem::StartingChildren, client_id);
          return false;
        }
        subsystem->NotifyParents();
        break;
      case Message::kReportOper:
        subsystem->GetLogger().Log(
            toolbelt::LogLevel::kInfo,
            "Subsystem %s has reported oper state change to %s",
            message->sender->Name().c_str(),
            OperStateName(message->state.oper));
        break;
      }
    }
    return true;
  });
}

void Subsystem::StartingChildren(uint32_t client_id, co::Coroutine *c) {
  if (children_.empty()) {
    EnterState(&Subsystem::StartingProcesses, client_id);
    return;
  }
  oper_state_ = OperState::kStartingChildren;
  SendToChildren(AdminState::kOnline, client_id);
  NotifyParents();

  capcom_.SendSubsystemStatusEvent(this);

  ProcessEvents(c, [ subsystem = shared_from_this(),
                     client_id ](EventSource event_source, co::Coroutine * c) {
    switch (event_source) {
    case EventSource::kStageZero: {
      // Event from stagezero.
      absl::StatusOr<stagezero::control::Event> event =
          subsystem->stagezero_client_->ReadEvent(c);
      if (!event.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError,
                                       "Failed to read event %s",
                                       event.status().ToString().c_str());
      }
      switch (event->event_case()) {
      case stagezero::control::Event::kStart: {
        break;
      }
      case stagezero::control::Event::kStop:
        return false;

      case stagezero::control::Event::kOutput:
        break;
      case stagezero::control::Event::EVENT_NOT_SET:
        break;
      }
      break;
    }
    case EventSource::kMessage: {
      // Incoming message.
      absl::StatusOr<Message> message = subsystem->ReadMessage();
      if (!message.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError, "%s",
                                       message.status().ToString().c_str());
      }
      switch (message->code) {
      case Message::kChangeAdmin:
        if (message->state.admin == AdminState::kOffline) {
          subsystem->active_clients_.Clear(client_id);
          if (subsystem->active_clients_.IsEmpty()) {
            subsystem->admin_state_ = AdminState::kOffline;
            // Request to go offline while we are starting our chilren.
            subsystem->EnterState(&Subsystem::StoppingProcesses, client_id);
            return false;
          }
        }
        subsystem->NotifyParents();

        break;
      case Message::kReportOper:
        subsystem->GetLogger().Log(
            toolbelt::LogLevel::kInfo,
            "Subsystem %s has reported oper state change to %s",
            message->sender->Name().c_str(),
            OperStateName(message->state.oper));
        break;
      }
      break;
    }
    case EventSource::kUnknown:
      break;
    }

    // If all our children are oper online, we can start our processes or go
    // directly online if there are no processes.
    for (auto &child : subsystem->children_) {
      if (child->oper_state_ != OperState::kOnline) {
        return true;
      }
    }
    subsystem->EnterState(&Subsystem::StartingProcesses, client_id);
    return false;
  });
}

void Subsystem::StartingProcesses(uint32_t client_id, co::Coroutine *c) {
  if (processes_.empty()) {
    EnterState(&Subsystem::Online, client_id);
    return;
  }
  oper_state_ = OperState::kStartingProcesses;
  LaunchProcesses(c);
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);

  ProcessEvents(c, [ subsystem = shared_from_this(),
                     client_id ](EventSource event_source, co::Coroutine * c) {
    switch (event_source) {
    case EventSource::kStageZero: {
      // Event from stagezero.
      absl::StatusOr<stagezero::control::Event> event =
          subsystem->stagezero_client_->ReadEvent(c);
      if (!event.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError,
                                       "Failed to read event %s",
                                       event.status().ToString().c_str());
      }
      switch (event->event_case()) {
      case stagezero::control::Event::kStart: {
        Process *proc = subsystem->FindProcess(event->start().process_id());
        if (proc != nullptr) {
          proc->SetRunning();
          proc->ClearAlarm(subsystem->capcom_);
        }
        break;
      }
      case stagezero::control::Event::kStop:
        // Process failed to start.
        subsystem->GetLogger().Log(toolbelt::LogLevel::kInfo,
                                   "Process %s has crashed, restarting",
                                   event->stop().process_id().c_str());
        subsystem->RestartIfPossible(event->stop().process_id(), client_id);
        return false;

      case stagezero::control::Event::kOutput:
        break;
      case stagezero::control::Event::EVENT_NOT_SET:
        break;
      }
      break;
    }
    case EventSource::kMessage: {
      // Incoming message.
      absl::StatusOr<Message> message = subsystem->ReadMessage();
      if (!message.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError, "%s",
                                       message.status().ToString().c_str());
      }
      switch (message->code) {
      case Message::kChangeAdmin:
        if (message->state.admin == AdminState::kOffline) {
          subsystem->active_clients_.Clear(client_id);
          if (subsystem->active_clients_.IsEmpty()) {
            subsystem->admin_state_ = AdminState::kOffline;

            // Request to go offline while we are starting our chilren.
            subsystem->EnterState(&Subsystem::StoppingProcesses, client_id);
            return false;
          }
        }
        subsystem->NotifyParents();
        break;
      case Message::kReportOper:
        subsystem->GetLogger().Log(
            toolbelt::LogLevel::kInfo,
            "Subsystem %s has reported oper state change to %s",
            message->sender->Name().c_str(),
            OperStateName(message->state.oper));
        break;
      }
      break;
    }
    case EventSource::kUnknown:
      break;
    }
    // If all our processes are running we can go online.
    if (!subsystem->AllProcessesRunning()) {
      return true;
    }

    subsystem->EnterState(&Subsystem::Online, client_id);
    return false;
  });
}

void Subsystem::Online(uint32_t client_id, co::Coroutine *c) {
  oper_state_ = OperState::kOnline;
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);

  ProcessEvents(c, [ subsystem = shared_from_this(),
                     client_id ](EventSource event_source, co::Coroutine * c) {
    switch (event_source) {
    case EventSource::kStageZero: {
      // Event from stagezero.
      absl::StatusOr<stagezero::control::Event> event =
          subsystem->stagezero_client_->ReadEvent(c);
      if (!event.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError,
                                       "Failed to read event %s",
                                       event.status().ToString().c_str());
      }
      switch (event->event_case()) {
      case stagezero::control::Event::kStart: {
        // We aren't going to get these as all processes are running.
        break;
      }
      case stagezero::control::Event::kStop: {
        // Process crashed.  Restart.
        subsystem->GetLogger().Log(toolbelt::LogLevel::kInfo,
                                   "Process %s has crashed, restarting",
                                   event->stop().process_id().c_str());
        subsystem->RestartIfPossible(event->stop().process_id(), client_id);
        return false;
      }
      case stagezero::control::Event::kOutput:
        break;
      case stagezero::control::Event::EVENT_NOT_SET:
        break;
      }
      break;
    }
    case EventSource::kMessage: {
      // Incoming message.
      absl::StatusOr<Message> message = subsystem->ReadMessage();
      if (!message.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError, "%s",
                                       message.status().ToString().c_str());
      }
      switch (message->code) {
      case Message::kChangeAdmin:
        if (message->state.admin == AdminState::kOffline) {
          subsystem->active_clients_.Clear(client_id);
          if (subsystem->active_clients_.IsEmpty()) {
            subsystem->admin_state_ = AdminState::kOffline;
            subsystem->EnterState(&Subsystem::StoppingProcesses, client_id);
            return false;
          }
        }
        subsystem->NotifyParents();
        break;
      case Message::kReportOper:
        subsystem->GetLogger().Log(
            toolbelt::LogLevel::kInfo,
            "Subsystem %s has reported oper state change to %s",
            message->sender->Name().c_str(),
            OperStateName(message->state.oper));
        break;
      }
      break;
    }
    case EventSource::kUnknown:
      break;
    }
    return true;
  });
}

void Subsystem::StoppingProcesses(uint32_t client_id, co::Coroutine *c) {
  if (processes_.empty()) {
    EnterState(&Subsystem::StoppingChildren, client_id);
    return;
  }
  oper_state_ = OperState::kStoppingProcesses;
  StopProcesses(c);
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);

  ProcessEvents(c, [ subsystem = shared_from_this(),
                     client_id ](EventSource event_source, co::Coroutine * c) {
    switch (event_source) {
    case EventSource::kStageZero: {
      // Event from stagezero.
      absl::StatusOr<stagezero::control::Event> event =
          subsystem->stagezero_client_->ReadEvent(c);
      if (!event.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError,
                                       "Failed to read event %s",
                                       event.status().ToString().c_str());
      }
      switch (event->event_case()) {
      case stagezero::control::Event::kStart: {
        // We aren't going to get these as all processes are stopping.
        break;
      }
      case stagezero::control::Event::kStop: {
        // Process stopped OK.
        Process *proc = subsystem->FindProcess(event->stop().process_id());
        if (proc != nullptr) {
          proc->SetStopped();
          subsystem->DeleteProcessId(proc->GetProcessId());
        }
        break;
      }
      case stagezero::control::Event::kOutput:
        break;
      case stagezero::control::Event::EVENT_NOT_SET:
        break;
      }
      break;
    }
    case EventSource::kMessage: {
      // Incoming message.
      absl::StatusOr<Message> message = subsystem->ReadMessage();
      if (!message.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError, "%s",
                                       message.status().ToString().c_str());
      }
      switch (message->code) {
      case Message::kChangeAdmin:
        if (message->state.admin == AdminState::kOffline) {
          // Request to go offline while we are starting our processes.
        }
        // TODO: report our oper state to the parents.

        break;
      case Message::kReportOper:
        subsystem->GetLogger().Log(
            toolbelt::LogLevel::kInfo,
            "Subsystem %s has reported oper state change to %s",
            message->sender->Name().c_str(),
            OperStateName(message->state.oper));
        break;
      }
      break;
    }
    case EventSource::kUnknown:
      break;
    }

    // If all our processes are stopped we can stop the children or go offline.
    if (!subsystem->AllProcessesStopped()) {
      return true;
    }
    // If we have children, stop them now, otherwise we are offline.
    subsystem->EnterState(&Subsystem::StoppingChildren, client_id);
    return false;
  });
}

void Subsystem::StoppingChildren(uint32_t client_id, co::Coroutine *c) {
  if (children_.empty()) {
    EnterState(&Subsystem::Offline, client_id);
    return;
  }
  oper_state_ = OperState::kStoppingChildren;
  SendToChildren(AdminState::kOffline, client_id);
  NotifyParents();

  capcom_.SendSubsystemStatusEvent(this);

  ProcessEvents(c, [ subsystem = shared_from_this(),
                     client_id ](EventSource event_source, co::Coroutine * c) {
    switch (event_source) {
    case EventSource::kStageZero: {
      // Event from stagezero.
      absl::StatusOr<stagezero::control::Event> event =
          subsystem->stagezero_client_->ReadEvent(c);
      if (!event.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError,
                                       "Failed to read event %s",
                                       event.status().ToString().c_str());
      }
      switch (event->event_case()) {
      case stagezero::control::Event::kStart: {
        // We aren't going to get these as all processes are stopping.
        break;
      }
      case stagezero::control::Event::kStop: {
        break;
      }
      case stagezero::control::Event::kOutput:
        break;
      case stagezero::control::Event::EVENT_NOT_SET:
        break;
      }
      break;
    }
    case EventSource::kMessage: {
      // Incoming message.
      absl::StatusOr<Message> message = subsystem->ReadMessage();
      if (!message.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError, "%s",
                                       message.status().ToString().c_str());
      }
      switch (message->code) {
      case Message::kChangeAdmin:
        if (message->state.admin == AdminState::kOffline) {
          // Request to go offline while we are starting our processes.
        }
        // TODO: report our oper state to the parents.

        break;
      case Message::kReportOper:
        subsystem->GetLogger().Log(
            toolbelt::LogLevel::kInfo,
            "Subsystem %s has reported oper state change to %s",
            message->sender->Name().c_str(),
            OperStateName(message->state.oper));
        break;
      }
      break;
    }
    case EventSource::kUnknown:
      break;
    }

    // If all our children are oper offline, we can go offline.
    for (auto &child : subsystem->children_) {
      if (child->oper_state_ != OperState::kOffline) {
        return true;
      }
    }

    subsystem->EnterState(&Subsystem::Offline, client_id);
    return false;
  });
}

void Subsystem::RestartNow(uint32_t client_id) {
  EnterState(&Subsystem::StartingChildren, client_id);
}

void Subsystem::RestartIfPossible(std::string process_id, uint32_t client_id) {
  Process *proc = FindProcess(process_id);
  if (proc != nullptr) {
    proc->SetStopped();
    DeleteProcessId(proc->GetProcessId());
  }

  if (num_restarts_ == max_restarts_) {
    // We have reached the max number of restarts, so we are broken.
    EnterState(&Subsystem::Broken, client_id);


    proc->RaiseAlarm(capcom_, {
      .name = proc->Name(), .type = Alarm::Type::kProcess,
      .severity = Alarm::Severity::kCritical, .reason = Alarm::Reason::kCrashed,
      .status = Alarm::Status::kRaised,
      .details = absl::StrFormat(
          "Process %s crashed too many times and is now broken", proc->Name())
    });
    return;
  }

  proc->RaiseAlarm(capcom_, {
    .name = proc->Name(), .type = Alarm::Type::kProcess,
    .severity = Alarm::Severity::kWarning, .reason = Alarm::Reason::kCrashed,
    .status = Alarm::Status::kRaised,
    .details = absl::StrFormat("Process %s has exited", proc->Name())
  });

  ++num_restarts_;
  EnterState(&Subsystem::Restarting, client_id);
}

void Subsystem::Restarting(uint32_t client_id, co::Coroutine *c) {
  std::cerr << "ENTERING RESTARTING STATE\n";
  oper_state_ = OperState::kRestarting;
  capcom_.SendSubsystemStatusEvent(this);
  if (processes_.empty()) {
    if (parents_.empty()) {
      // We have no processes or parents, restart now.
      RestartNow(client_id);
      return;
    }
    // We have no processes, but parents exist, notify them of the restart.
    NotifyParents();
    return;
  }

  // If all our processes are stopped, restart now.
  if (AllProcessesStopped() && parents_.empty()) {
    // We have no parents.  Restart now and leave this state.
    RestartNow(client_id);
    return;
  }

  // Stopping processes.
  StopProcesses(c);

  // Wait for all our processes to stop, then notify the parents that
  // we've stopped and are ready to be restarted.
  ProcessEvents(c, [ subsystem = shared_from_this(),
                     client_id ](EventSource event_source, co::Coroutine * c) {
    switch (event_source) {
    case EventSource::kStageZero: {
      // Event from stagezero.
      absl::StatusOr<stagezero::control::Event> event =
          subsystem->stagezero_client_->ReadEvent(c);
      if (!event.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError,
                                       "Failed to read event %s",
                                       event.status().ToString().c_str());
      }
      switch (event->event_case()) {
      case stagezero::control::Event::kStart: {
        // This might happen if a process crashed while others are
        // starting up.  Ignore it, as it will be replaced by a kStop
        // event when the process is killed.
        break;
      }
      case stagezero::control::Event::kStop: {
        Process *proc = subsystem->FindProcess(event->stop().process_id());
        if (proc != nullptr) {
          proc->SetStopped();
          subsystem->DeleteProcessId(proc->GetProcessId());
        }
        break;
      }
      case stagezero::control::Event::kOutput:
        break;
      case stagezero::control::Event::EVENT_NOT_SET:
        break;
      }
      break;
    }
    case EventSource::kMessage: {
      // Incoming message.
      absl::StatusOr<Message> message = subsystem->ReadMessage();
      if (!message.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError, "%s",
                                       message.status().ToString().c_str());
      }
      switch (message->code) {
      case Message::kChangeAdmin:
        if (message->state.admin == AdminState::kOnline) {
          // We are restarting and have been asked to go online.
          subsystem->RestartNow(client_id);
          break;
        }
        subsystem->NotifyParents();
        break;
      case Message::kReportOper:
        // Notification from a child while in restarting state.  We shouldn't
        // get this.
        subsystem->GetLogger().Log(toolbelt::LogLevel::kError,
                                   "Subsystem %s has reported oper state "
                                   "change to %s while in restarting state",
                                   message->sender->Name().c_str(),
                                   OperStateName(message->state.oper));
        break;
      }
      break;
    }
    case EventSource::kUnknown:
      break;
    }
    // If all our processes are not stopped we can notify our parents.  We stay
    // in this state.
    if (!subsystem->AllProcessesStopped()) {
      return true;
    }

    // All our processes are down.  Notify the parents that we have stopped
    // everything and are in kRestarting state.
    if (subsystem->parents_.empty()) {
      // We have no parents.  Restart now and leave this state.
      subsystem->RestartNow(client_id);
      return false;
    }
    // Notify the parents and stay in kRestarting state.
    subsystem->NotifyParents();
    return true;
  });
}

void Subsystem::Broken(uint32_t client_id, co::Coroutine *c) {
  oper_state_ = OperState::kBroken;
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);
  ProcessEvents(c, [ subsystem = shared_from_this(),
                     client_id ](EventSource event_source, co::Coroutine * c) {
    if (event_source == EventSource::kMessage) {
      // Incoming message.
      absl::StatusOr<Message> message = subsystem->ReadMessage();
      if (!message.ok()) {
        subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError, "%s",
                                       message.status().ToString().c_str());
      }
      switch (message->code) {
      case Message::kChangeAdmin:
        subsystem->num_restarts_ = 0; // Reset restart counter.
        if (message->state.admin == AdminState::kOnline) {
          subsystem->active_clients_.Set(client_id);
          subsystem->EnterState(&Subsystem::StartingChildren, client_id);
        } else {
          // Stop all children.
          subsystem->active_clients_.Clear(client_id);
          subsystem->EnterState(&Subsystem::StoppingChildren, client_id);
        }
        return false;
      case Message::kReportOper:
        subsystem->GetLogger().Log(toolbelt::LogLevel::kInfo,
                                   "Subsystem %s has reported oper state "
                                   "change to %s while it is broken",
                                   message->sender->Name().c_str(),
                                   OperStateName(message->state.oper));
        break;
      }
    }
    return true;
  });
}

void Subsystem::RaiseAlarm(const Alarm& alarm) {
  alarm_ = alarm;
  capcom_.SendAlarm(alarm_);
  alarm_raised_ = true;
}

void Subsystem::ClearAlarm() {
  if (!alarm_raised_) {
    return;
  }
  alarm_.status = Alarm::Status::kCleared;
  capcom_.SendAlarm(alarm_);
  alarm_raised_ = false;
}

absl::Status
Subsystem::AddStaticProcess(const stagezero::config::StaticProcess &proc,
                            const stagezero::config::ProcessOptions &options) {
  auto p = std::make_unique<StaticProcess>(options.name(), proc.executable(),
                                           options);
  processes_.push_back(std::move(p));

  return absl::OkStatus();
}

void Subsystem::BuildStatusEvent(capcom::proto::SubsystemStatus *event) {
  event->set_name(name_);

  switch (admin_state_) {
  case AdminState::kOffline:
    event->set_admin_state(capcom::proto::ADMIN_OFFLINE);
    break;
  case AdminState::kOnline:
    event->set_admin_state(capcom::proto::ADMIN_ONLINE);
    break;
  }

  switch (oper_state_) {
  case OperState::kOffline:
    event->set_oper_state(capcom::proto::OPER_OFFLINE);
    break;
  case OperState::kStartingChildren:
    event->set_oper_state(capcom::proto::OPER_STARTING_CHILDREN);
    break;
  case OperState::kStartingProcesses:
    event->set_oper_state(capcom::proto::OPER_STARTING_PROCESSES);
    break;
  case OperState::kOnline:
    event->set_oper_state(capcom::proto::OPER_ONLINE);
    break;
  case OperState::kStoppingChildren:
    event->set_oper_state(capcom::proto::OPER_STOPPING_CHILDREN);
    break;
  case OperState::kStoppingProcesses:
    event->set_oper_state(capcom::proto::OPER_STOPPING_PROCESSES);
    break;
  case OperState::kRestarting:
    event->set_oper_state(capcom::proto::OPER_RESTARTING);
    break;
  case OperState::kBroken:
    event->set_oper_state(capcom::proto::OPER_BROKEN);
    break;
  }

  // Processes.
  for (auto &proc : processes_) {
    auto *p = event->add_processes();
    p->set_name(proc->Name());
    p->set_process_id(proc->GetProcessId());
    p->set_pid(proc->GetPid());
    p->set_running(proc->IsRunning());
  }
}

void Process::ParseOptions(const stagezero::config::ProcessOptions &options) {
  description_ = options.description();
  for (auto &var : options.vars()) {
    vars_.push_back({var.name(), var.value(), var.exported()});
  }
  startup_timeout_secs_ = options.startup_timeout_secs();
  sigint_shutdown_timeout_secs_ = options.sigint_shutdown_timeout_secs();
  sigterm_shutdown_timeout_secs_ = options.sigterm_shutdown_timeout_secs();
  notify_ = options.notify();
}

absl::Status Process::Stop(stagezero::Client &client, co::Coroutine *c) {
  return client.StopProcess(process_id_, c);
}

void Process::RaiseAlarm(Capcom &capcom, const Alarm& alarm) {
  alarm_ = alarm;
  capcom.SendAlarm(alarm_);
  alarm_raised_ = true;
}

void Process::ClearAlarm(Capcom &capcom) {
  if (!alarm_raised_) {
    return;
  }
  alarm_.status = Alarm::Status::kCleared;
  capcom.SendAlarm(alarm_);
  alarm_raised_ = false;
}

StaticProcess::StaticProcess(std::string name, std::string executable,
                             const stagezero::config::ProcessOptions &options)
    : Process(std::move(name)), executable_(std::move(executable)) {
  ParseOptions(options);
}

absl::Status StaticProcess::Launch(stagezero::Client &client,
                                   co::Coroutine *c) {
  stagezero::ProcessOptions options = {
      .description = description_,
      .args = args_,
      .startup_timeout_secs = startup_timeout_secs_,
      .sigint_shutdown_timeout_secs = sigint_shutdown_timeout_secs_,
      .sigterm_shutdown_timeout_secs = sigterm_shutdown_timeout_secs_,
      .notify = notify_,
  };
  for (auto &var : vars_) {
    options.vars.push_back({var.name, var.value, var.exported});
  }
  // TODO: streams

  absl::StatusOr<std::pair<std::string, int>> s =
      client.LaunchStaticProcess(Name(), executable_, options, c);
  if (!s.ok()) {
    return s.status();
  }
  process_id_ = s->first;
  pid_ = s->second;
  return absl::OkStatus();
}

absl::Status VirtualProcess::Launch(stagezero::Client &client,
                                    co::Coroutine *c) {
  return absl::OkStatus();
}

} // namespace stagezero::capcom
