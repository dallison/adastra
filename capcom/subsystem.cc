#include "stagezero/capcom/subsystem.h"
#include "stagezero/capcom/capcom.h"

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
        subsys->oper_state_ = OperState::kOffline;
        subsys->capcom_.SendSubsystemStatusEvent(subsys);
        subsys->EnterState(&Subsystem::Offline);

        // This coroutine now exits, but the Offline coroutine is running,
        // listening for events.
      });
  capcom_.AddCoroutine(std::unique_ptr<co::Coroutine>(runner));
}

void Subsystem::Stop() {
  std::cerr << "triggering interrupt on " << Name() << std::endl;
  running_ = false;
  interrupt_.Trigger();
}

absl::Status Subsystem::Remove(bool recursive) {
  auto subsys = shared_from_this();
  // TODO: recursion.
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

    absl::Status status =
        stagezero_client_->StopProcess(proc->GetProcessId(), c);
    if (!status.ok()) {
      capcom_.logger_.Log(toolbelt::LogLevel::kError,
                          "Failed to stop process %s: %s", proc->Name().c_str(),
                          status.ToString().c_str());
      continue;
    }
    DeleteProcessId(proc->GetProcessId());
  }
}

void Subsystem::EnterState(
    std::function<void(std::shared_ptr<Subsystem>, co::Coroutine *)> func) {

  co::Coroutine *coroutine = new co::Coroutine(Scheduler(), [
    subsystem = shared_from_this(), func = std::move(func)
  ](co::Coroutine * c) { func(subsystem, c); });

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

void Subsystem::Offline(co::Coroutine *c) {
  ProcessEvents(c, [subsystem = shared_from_this()](EventSource event_source,
                                                    co::Coroutine * c) {
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
          subsystem->EnterState(&Subsystem::StartingProcesses);
          return false;
        }
        // TODO: report our oper state to the parents.

        break;
      case Message::kReportOper:
        break;
      }
    }
    return true;
  });
}

void Subsystem::StartingProcesses(co::Coroutine *c) {
  oper_state_ = OperState::kStartingProcesses;
  LaunchProcesses(c);

  ProcessEvents(c, [subsystem = shared_from_this()](EventSource event_source,
                                                    co::Coroutine * c) {
    subsystem->capcom_.SendSubsystemStatusEvent(subsystem);
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
        }
        break;
      }
      case stagezero::control::Event::kStop:
        // Process failed to start.
        // TODO: Stop all processes and go into restarting state.
        break;

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
        break;
      }
      break;
    }
    case EventSource::kUnknown:
      break;
    }
    // If all our processes are running we can go online.
    for (auto &p : subsystem->processes_) {
      if (!p->IsRunning()) {
        return true;
      }
    }
    subsystem->EnterState(&Subsystem::Online);
    return false;
  });
}

void Subsystem::Online(co::Coroutine *c) {
  oper_state_ = OperState::kOnline;

  ProcessEvents(c, [subsystem = shared_from_this()](EventSource event_source,
                                                    co::Coroutine * c) {
    subsystem->capcom_.SendSubsystemStatusEvent(subsystem);
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
          subsystem->admin_state_ = AdminState::kOnline;
          subsystem->EnterState(&Subsystem::StoppingProcesses);
        }
        // TODO: report our oper state to the parents.

        break;
      case Message::kReportOper:
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

void Subsystem::StoppingProcesses(co::Coroutine *c) {
  oper_state_ = OperState::kStoppingProcesses;
  StopProcesses(c);

  ProcessEvents(c, [subsystem = shared_from_this()](EventSource event_source,
                                                    co::Coroutine * c) {
    subsystem->capcom_.SendSubsystemStatusEvent(subsystem);
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
        Process *proc = subsystem->FindProcess(event->start().process_id());
        if (proc != nullptr) {
          proc->SetRunning();
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
        break;
      }
      break;
    }
    case EventSource::kUnknown:
      break;
    }
    // If all our processes are stopped we can go offline.
    for (auto &p : subsystem->processes_) {
      if (p->IsRunning()) {
        return true;
      }
    }
    // TODO: stop children instead.
    subsystem->EnterState(&Subsystem::Offline);
    return false;
  });
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
