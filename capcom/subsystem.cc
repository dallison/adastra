// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

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

absl::StatusOr<std::shared_ptr<stagezero::Client>>
Subsystem::ConnectToStageZero(const Compute *compute, co::Coroutine *c) {
  auto &sc = computes_[compute->name];
  if (sc == nullptr) {
    auto client = std::make_shared<stagezero::Client>();
    if (absl::Status status =
            client->Init(compute->addr, name_, compute->name, c);
        !status.ok()) {
      return status;
    }
    sc = std::move(client);
  }
  return sc;
}

void Subsystem::Run() {
  // Run the subsystem in offline state.
  running_ = true;
  admin_state_ = AdminState::kOffline;
  EnterState(OperState::kOffline, kNoClient);
  capcom_.logger_.Log(toolbelt::LogLevel::kInfo, "Subsystem %s is now active",
                      Name().c_str());
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
    std::cerr << "Removing subsystem process " << proc->Name() << " IsZygote "
              << proc->IsZygote() << std::endl;
    if (proc->IsZygote()) {
      if (absl::Status status = subsys->capcom_.RemoveZygote(proc->Name());
          !status.ok()) {
        return status;
      }
    }
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

absl::Status Subsystem::LaunchProcesses(co::Coroutine *c) {
  std::cerr << "subsystem " << Name()
            << " launching processes: " << processes_.size() << std::endl;
  for (auto &proc : processes_) {
    std::cerr << "process " << proc->Name() << std::endl;
    absl::Status status = proc->Launch(c);
    if (!status.ok()) {
      // A failure to launch one is a failure for all.
      return status;
    }
    RecordProcessId(proc->GetProcessId(), proc.get());
  }
  return absl::OkStatus();
}

void Subsystem::StopProcesses(co::Coroutine *c) {
  for (auto &proc : processes_) {

    absl::Status status = proc->Stop(c);
    if (!status.ok()) {
      capcom_.logger_.Log(toolbelt::LogLevel::kError,
                          "Failed to stop process %s: %s", proc->Name().c_str(),
                          status.ToString().c_str());
      continue;
    }
  }
}

// NOTE: Keep this array up to date with OperState.
std::function<void(std::shared_ptr<Subsystem>, uint32_t, co::Coroutine *)>
    Subsystem::state_funcs_[] = {
        &Subsystem::Offline,           &Subsystem::StartingChildren,
        &Subsystem::StartingProcesses, &Subsystem::Online,
        &Subsystem::StoppingProcesses, &Subsystem::StoppingChildren,
        &Subsystem::Restarting,        &Subsystem::Broken,
};

void Subsystem::EnterState(OperState state, uint32_t client_id) {
  std::string coroutine_name =
      absl::StrFormat("%s/%s", Name(), OperStateName(state));
  std::cerr << "subsystem " << Name() << " entering state "
            << OperStateName(state) << std::endl;
  co::Coroutine *coroutine = new co::Coroutine(
      Scheduler(),
      [ subsystem = shared_from_this(), state, client_id ](co::Coroutine * c) {
        state_funcs_[static_cast<int>(state)](subsystem, client_id, c);
      },
      coroutine_name);

  capcom_.AddCoroutine(std::unique_ptr<co::Coroutine>(coroutine));
}

void Subsystem::RunSubsystemInState(
    co::Coroutine *c,
    std::function<StateTransition(
        EventSource, std::shared_ptr<stagezero::Client>, co::Coroutine *)>
        handler) {
  auto subsystem = shared_from_this();

  std::vector<struct pollfd> fds;
  for (auto & [ name, client ] : computes_) {
    fds.push_back({client->GetEventFd().Fd(), POLLIN});
  }
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
    std::shared_ptr<stagezero::Client> found_client;
    if (fd == subsystem->incoming_message_.Fd()) {
      event_source = EventSource::kMessage;
    } else {
      // Find the client associated with the fd.  There will be about
      // one or two clients per subsystem, most likelu, so a linear
      // search if fine.
      for (auto & [ name, client ] : computes_) {
        if (fd == client->GetEventFd().Fd()) {
          found_client = client;
          event_source = EventSource::kStageZero;
          break;
        }
      }
    }
    if (event_source != EventSource::kUnknown) {
      if (handler(event_source, std::move(found_client), c) ==
          StateTransition::kLeave) {
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
  RunSubsystemInState(
      c, [subsystem = shared_from_this()](
             EventSource event_source,
             std::shared_ptr<stagezero::Client> stagezero_client,
             co::Coroutine * c)
             ->StateTransition {
               switch (event_source) {
               case EventSource::kStageZero: {
                 // Event from stagezero.
                 absl::StatusOr<std::shared_ptr<stagezero::control::Event>> e =
                     stagezero_client->ReadEvent(c);
                 if (!e.ok()) {
                   subsystem->capcom_.logger_.Log(
                       toolbelt::LogLevel::kError, "Failed to read event %s",
                       e.status().ToString().c_str());
                 }
                 std::shared_ptr<stagezero::control::Event> event = *e;
                 switch (event->event_case()) {
                 case stagezero::control::Event::kStart: {
                   break;
                 }
                 case stagezero::control::Event::kStop:
                 case stagezero::control::Event::kOutput:
                   break;
                 case stagezero::control::Event::kLog:
                   subsystem->capcom_.Log(event->log());
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
                   subsystem->capcom_.logger_.Log(
                       toolbelt::LogLevel::kError, "%s",
                       message.status().ToString().c_str());
                 }
                 switch (message->code) {
                 case Message::kChangeAdmin:
                   std::cerr << "Subsystem " << subsystem->Name()
                             << " got admin change in "
                             << AdminStateName(subsystem->admin_state_)
                             << " from client " << message->client_id
                             << std::endl;
                   if (message->state.admin == AdminState::kOnline) {
                     subsystem->admin_state_ = AdminState::kOnline;
                     if (message->client_id != kNoClient) {
                       subsystem->active_clients_.Set(message->client_id);
                     }
                     subsystem->active_clients_.Print();
                     subsystem->EnterState(OperState::kStartingChildren,
                                           message->client_id);
                     return StateTransition::kLeave;
                   } else {
                     // No state change, but client might be waiting for an
                     // event.
                     subsystem->capcom_.SendSubsystemStatusEvent(
                         subsystem.get());
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
                 case Message::kAbort:
                   break;
                 }
                 break;
               }
               case EventSource::kUnknown:
                 break;
               }

               return StateTransition::kStay;
             });
}

void Subsystem::StartingChildren(uint32_t client_id, co::Coroutine *c) {
  if (children_.empty()) {
    std::cerr << "Subsystem " << Name() << " has no children\n";
    EnterState(OperState::kStartingProcesses, client_id);
    return;
  }
  oper_state_ = OperState::kStartingChildren;
  SendToChildren(AdminState::kOnline, client_id);
  NotifyParents();

  capcom_.SendSubsystemStatusEvent(this);

  // Mapping to hold whether child has notified.
  absl::flat_hash_map<Subsystem *, bool> child_notified;
  for (auto &child : children_) {
    child_notified.insert(std::make_pair(child.get(), false));
  }

  RunSubsystemInState(
      c,
      [ subsystem = shared_from_this(), client_id,
        &child_notified ](EventSource event_source,
                          std::shared_ptr<stagezero::Client> stagezero_client,
                          co::Coroutine * c)
          ->StateTransition {
            switch (event_source) {
            case EventSource::kStageZero: {
              // Event from stagezero.
              absl::StatusOr<std::shared_ptr<stagezero::control::Event>> e =
                  stagezero_client->ReadEvent(c);
              if (!e.ok()) {
                subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError,
                                               "Failed to read event %s",
                                               e.status().ToString().c_str());
              }
              std::shared_ptr<stagezero::control::Event> event = *e;

              switch (event->event_case()) {
              case stagezero::control::Event::kStart: {
                break;
              }
              case stagezero::control::Event::kStop:
                // One of our processes crashed while starting the children.
                // Since nothing should be running this is a late message.
                // Igore it.
                return StateTransition::kStay;

              case stagezero::control::Event::kOutput:
                break;
              case stagezero::control::Event::kLog:
                subsystem->capcom_.Log(event->log());
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
                subsystem->capcom_.logger_.Log(
                    toolbelt::LogLevel::kError, "%s",
                    message.status().ToString().c_str());
              }
              switch (message->code) {
              case Message::kChangeAdmin:
                if (message->state.admin == AdminState::kOffline) {
                  if (client_id != kNoClient) {
                    subsystem->active_clients_.Clear(client_id);

                    if (subsystem->active_clients_.IsEmpty()) {
                      subsystem->admin_state_ = AdminState::kOffline;
                      // Request to go offline while we are starting our
                      // chilren.
                      subsystem->EnterState(OperState::kStoppingProcesses,
                                            client_id);
                      return StateTransition::kLeave;
                    }
                  }
                } else {
                  // No state change, but client might be waiting for an event.
                  subsystem->capcom_.SendSubsystemStatusEvent(subsystem.get());
                }
                subsystem->NotifyParents();

                break;
              case Message::kReportOper:
                subsystem->GetLogger().Log(
                    toolbelt::LogLevel::kInfo,
                    "Subsystem %s has reported oper state change to %s",
                    message->sender->Name().c_str(),
                    OperStateName(message->state.oper));
                child_notified[message->sender] = true;
                break;
              case Message::kAbort:
                subsystem->Abort();
                return StateTransition::kLeave;
              }
              break;
            }
            case EventSource::kUnknown:
              break;
            }

            for (auto & [ child, notified ] : child_notified) {
              if (!notified) {
                return StateTransition::kStay;
              }
            }

            // We only start when the children are online.
            for (auto &child : subsystem->children_) {
              if (child->oper_state_ != OperState::kOnline) {
                return StateTransition::kStay;
              }
            }
            subsystem->EnterState(OperState::kStartingProcesses, client_id);
            return StateTransition::kLeave;
          });
}

void Subsystem::StartingProcesses(uint32_t client_id, co::Coroutine *c) {
  if (processes_.empty()) {
    EnterState(OperState::kOnline, client_id);
    return;
  }
  oper_state_ = OperState::kStartingProcesses;
  if (absl::Status status = LaunchProcesses(c); !status.ok()) {
    // If we fail to lauch, go into restarting state if we can.  if we
    // can't (due to number of attempts) we go into broken state.
    RestartIfPossible(client_id, c);
    return;
  }
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);

  RunSubsystemInState(
      c,
      [ subsystem = shared_from_this(),
        client_id ](EventSource event_source,
                    std::shared_ptr<stagezero::Client> stagezero_client,
                    co::Coroutine * c)
          ->StateTransition {
            switch (event_source) {
            case EventSource::kStageZero: {
              // Event from stagezero.
              absl::StatusOr<std::shared_ptr<stagezero::control::Event>> e =
                  stagezero_client->ReadEvent(c);
              if (!e.ok()) {
                subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError,
                                               "Failed to read event %s",
                                               e.status().ToString().c_str());
              }
              std::shared_ptr<stagezero::control::Event> event = *e;
              switch (event->event_case()) {
              case stagezero::control::Event::kStart: {
                Process *proc =
                    subsystem->FindProcess(event->start().process_id());
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
                subsystem->RestartIfPossibleAfterProcessCrash(
                    event->stop().process_id(), client_id, c);
                return StateTransition::kLeave;

              case stagezero::control::Event::kOutput:
                break;
              case stagezero::control::Event::kLog:
                subsystem->capcom_.Log(event->log());
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
                subsystem->capcom_.logger_.Log(
                    toolbelt::LogLevel::kError, "%s",
                    message.status().ToString().c_str());
              }
              switch (message->code) {
              case Message::kChangeAdmin:
                if (message->state.admin == AdminState::kOffline) {
                  subsystem->active_clients_.Clear(client_id);
                  if (subsystem->active_clients_.IsEmpty()) {
                    subsystem->admin_state_ = AdminState::kOffline;

                    // Request to go offline while we are starting our chilren.
                    subsystem->EnterState(OperState::kStoppingProcesses,
                                          client_id);
                    return StateTransition::kLeave;
                  }
                } else {
                  // No state change, but client might be waiting for an event.
                  subsystem->capcom_.SendSubsystemStatusEvent(subsystem.get());
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
              case Message::kAbort:
                subsystem->Abort();
                return StateTransition::kLeave;
              }
              break;
            }
            case EventSource::kUnknown:
              break;
            }
            // If all our processes are running we can go online.
            if (!subsystem->AllProcessesRunning()) {
              return StateTransition::kStay;
            }

            subsystem->EnterState(OperState::kOnline, client_id);
            return StateTransition::kLeave;
          });
}

void Subsystem::Online(uint32_t client_id, co::Coroutine *c) {
  oper_state_ = OperState::kOnline;
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);

  RunSubsystemInState(
      c,
      [ subsystem = shared_from_this(),
        client_id ](EventSource event_source,
                    std::shared_ptr<stagezero::Client> stagezero_client,
                    co::Coroutine * c)
          ->StateTransition {
            switch (event_source) {
            case EventSource::kStageZero: {
              // Event from stagezero.
              absl::StatusOr<std::shared_ptr<stagezero::control::Event>> e =
                  stagezero_client->ReadEvent(c);
              if (!e.ok()) {
                subsystem->capcom_.logger_.Log(toolbelt::LogLevel::kError,
                                               "Failed to read event %s",
                                               e.status().ToString().c_str());
              }
              std::shared_ptr<stagezero::control::Event> event = *e;
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
                subsystem->RestartIfPossibleAfterProcessCrash(
                    event->stop().process_id(), client_id, c);
                return StateTransition::kLeave;
              }
              case stagezero::control::Event::kOutput:
                break;
              case stagezero::control::Event::kLog:
                subsystem->capcom_.Log(event->log());
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
                subsystem->capcom_.logger_.Log(
                    toolbelt::LogLevel::kError, "%s",
                    message.status().ToString().c_str());
              }
              switch (message->code) {
              case Message::kChangeAdmin:
                if (message->state.admin == AdminState::kOffline) {
                  std::cerr << subsystem->Name() << ": Clearing client id "
                            << message->client_id << " from active set\n";
                  subsystem->active_clients_.Print();

                  subsystem->active_clients_.Clear(message->client_id);
                  if (subsystem->active_clients_.IsEmpty()) {
                    subsystem->active_clients_.Print();

                    std::cerr << "No active clients\n";
                    subsystem->admin_state_ = AdminState::kOffline;
                    subsystem->EnterState(OperState::kStoppingProcesses,
                                          message->client_id);
                    return StateTransition::kLeave;
                  }
                } else {
                  if (message->client_id != kNoClient) {
                    // Request to go online while already online,  Add the
                    // client id to the active clients set.
                    subsystem->active_clients_.Set(message->client_id);
                  }
                  // No state change, but client might be waiting for an event.
                  subsystem->capcom_.SendSubsystemStatusEvent(subsystem.get());
                }
                subsystem->NotifyParents();
                break;
              case Message::kReportOper:
                subsystem->GetLogger().Log(
                    toolbelt::LogLevel::kInfo,
                    "Subsystem %s has reported oper state change to %s",
                    message->sender->Name().c_str(),
                    OperStateName(message->state.oper));
                if (message->state.oper == OperState::kRestarting) {
                  // Child has entered restarting state.  This is our signal
                  // to do that too.
                  subsystem->EnterState(OperState::kRestarting, client_id);
                  return StateTransition::kLeave;
                }
                break;
              case Message::kAbort:
                subsystem->Abort();
                return StateTransition::kLeave;
              }
              break;
            }
            case EventSource::kUnknown:
              break;
            }
            return StateTransition::kStay;
          });
}

void Subsystem::StoppingProcesses(uint32_t client_id, co::Coroutine *c) {
  if (processes_.empty()) {
    EnterState(OperState::kStoppingChildren, client_id);
    return;
  }
  oper_state_ = OperState::kStoppingProcesses;
  StopProcesses(c);
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);

  RunSubsystemInState(
      c, [ subsystem = shared_from_this(),
           client_id ](EventSource event_source,
                       std::shared_ptr<stagezero::Client> stagezero_client,
                       co::Coroutine * c)
             ->StateTransition {
               switch (event_source) {
               case EventSource::kStageZero: {
                 // Event from stagezero.
                 absl::StatusOr<std::shared_ptr<stagezero::control::Event>> e =
                     stagezero_client->ReadEvent(c);
                 if (!e.ok()) {
                   subsystem->capcom_.logger_.Log(
                       toolbelt::LogLevel::kError, "Failed to read event %s",
                       e.status().ToString().c_str());
                 }
                 std::shared_ptr<stagezero::control::Event> event = *e;
                 switch (event->event_case()) {
                 case stagezero::control::Event::kStart: {
                   // We aren't going to get these as all processes are
                   // stopping.
                   break;
                 }
                 case stagezero::control::Event::kStop: {
                   // Process stopped OK.
                   Process *proc =
                       subsystem->FindProcess(event->stop().process_id());
                   if (proc != nullptr) {
                     proc->SetStopped();
                     subsystem->DeleteProcessId(proc->GetProcessId());
                   }
                   break;
                 }
                 case stagezero::control::Event::kOutput:
                   break;
                 case stagezero::control::Event::kLog:
                   subsystem->capcom_.Log(event->log());
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
                   subsystem->capcom_.logger_.Log(
                       toolbelt::LogLevel::kError, "%s",
                       message.status().ToString().c_str());
                 }
                 switch (message->code) {
                 case Message::kChangeAdmin:
                   if (message->state.admin == AdminState::kOffline) {
                     // Request to go offline while we are stopping our
                     // processes.
                   } else {
                     // Request to go online while stopping processes.  Go back
                     // online by starting children again.
                     subsystem->EnterState(OperState::kStartingChildren,
                                           message->client_id);
                     return StateTransition::kLeave;
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
                 case Message::kAbort:
                   subsystem->Abort();
                   return StateTransition::kLeave;
                 }
                 break;
               }
               case EventSource::kUnknown:
                 break;
               }

               // If all our processes are stopped we can stop the children or
               // go offline.
               if (!subsystem->AllProcessesStopped()) {
                 return StateTransition::kStay;
               }
               // If we have children, stop them now, otherwise we are offline.
               subsystem->EnterState(OperState::kStoppingChildren, client_id);
               return StateTransition::kLeave;
             });
}

void Subsystem::StoppingChildren(uint32_t client_id, co::Coroutine *c) {
  if (children_.empty()) {
    EnterState(OperState::kOffline, client_id);
    return;
  }
  oper_state_ = OperState::kStoppingChildren;
  SendToChildren(AdminState::kOffline, client_id);
  NotifyParents();

  capcom_.SendSubsystemStatusEvent(this);

  // Mapping to hold whether child has notified.
  absl::flat_hash_map<Subsystem *, bool> child_notified;
  for (auto &child : children_) {
    child_notified.insert(std::make_pair(child.get(), false));
  }

  RunSubsystemInState(
      c, [ subsystem = shared_from_this(), client_id, &child_notified ](
             EventSource event_source,
             std::shared_ptr<stagezero::Client> stagezero_client,
             co::Coroutine * c)
             ->StateTransition {
               switch (event_source) {
               case EventSource::kStageZero: {
                 // Event from stagezero.
                 absl::StatusOr<std::shared_ptr<stagezero::control::Event>> e =
                     stagezero_client->ReadEvent(c);
                 if (!e.ok()) {
                   subsystem->capcom_.logger_.Log(
                       toolbelt::LogLevel::kError, "Failed to read event %s",
                       e.status().ToString().c_str());
                 }
                 std::shared_ptr<stagezero::control::Event> event = *e;
                 switch (event->event_case()) {
                 case stagezero::control::Event::kStart: {
                   // We shouldn't get this because all our processes are
                   // stopped.
                   break;
                 }
                 case stagezero::control::Event::kStop: {
                   // We shouldn't get this because all our processes are
                   // stopped.
                   break;
                 }
                 case stagezero::control::Event::kOutput:
                   break;
                 case stagezero::control::Event::kLog:
                   subsystem->capcom_.Log(event->log());
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
                   subsystem->capcom_.logger_.Log(
                       toolbelt::LogLevel::kError, "%s",
                       message.status().ToString().c_str());
                 }
                 switch (message->code) {
                 case Message::kChangeAdmin:
                   if (message->state.admin == AdminState::kOnline) {
                     // Request to go online while we are stopping childrem.
                   }
                   // TODO: report our oper state to the parents.

                   break;
                 case Message::kReportOper:
                   subsystem->GetLogger().Log(
                       toolbelt::LogLevel::kInfo,
                       "Subsystem %s has reported oper state change to %s",
                       message->sender->Name().c_str(),
                       OperStateName(message->state.oper));
                   child_notified[message->sender] = true;
                   break;
                 case Message::kAbort:
                   subsystem->Abort();
                   return StateTransition::kLeave;
                 }
                 break;
               }
               case EventSource::kUnknown:
                 break;
               }

               for (auto & [ child, notified ] : child_notified) {
                 if (!notified) {
                   return StateTransition::kStay;
                 }
               }

               for (auto &child : subsystem->children_) {
                 if (child->oper_state_ != OperState::kOffline) {
                   return StateTransition::kStay;
                 }
               }
               subsystem->EnterState(OperState::kOffline, client_id);
               return StateTransition::kLeave;
             });
}

void Subsystem::RestartNow(uint32_t client_id) {
  EnterState(OperState::kStartingChildren, client_id);
}

void Subsystem::RestartIfPossibleAfterProcessCrash(std::string process_id,
                                                   uint32_t client_id,
                                                   co::Coroutine *c) {
  Process *proc = FindProcess(process_id);
  if (proc == nullptr) {
    GetLogger().Log(toolbelt::LogLevel::kError,
                    "Cannot find process %s for restart", process_id.c_str());
    return;
  }
  proc->SetStopped();
  DeleteProcessId(proc->GetProcessId());

  if (num_restarts_ == max_restarts_) {
    // We have reached the max number of restarts, so we are broken.
    EnterState(OperState::kBroken, client_id);

    proc->RaiseAlarm(capcom_,
                     {.name = proc->Name(),
                      .type = Alarm::Type::kProcess,
                      .severity = Alarm::Severity::kCritical,
                      .reason = Alarm::Reason::kCrashed,
                      .status = Alarm::Status::kRaised,
                      .details = absl::StrFormat(
                          "Process %s crashed too many times and is now broken",
                          proc->Name())});
    return;
  }

  proc->RaiseAlarm(capcom_, {.name = proc->Name(),
                             .type = Alarm::Type::kProcess,
                             .severity = Alarm::Severity::kWarning,
                             .reason = Alarm::Reason::kCrashed,
                             .status = Alarm::Status::kRaised,
                             .details = absl::StrFormat("Process %s has exited",
                                                        proc->Name())});

  // Delay before restarting.
  restart_delay_ = std::min(kMaxRestartDelay, restart_delay_ * 2);
  ++num_restarts_;
  c->Sleep(restart_delay_.count());
  EnterState(OperState::kRestarting, client_id);
}

void Subsystem::RestartIfPossible(uint32_t client_id, co::Coroutine *c) {
  if (num_restarts_ == max_restarts_) {
    // We have reached the max number of restarts, so we are broken.
    EnterState(OperState::kBroken, client_id);
    return;
  }
  // Delay before restarting.
  restart_delay_ = std::min(kMaxRestartDelay, restart_delay_ * 2);
  ++num_restarts_;
  c->Sleep(restart_delay_.count());
  EnterState(OperState::kRestarting, client_id);
}

void Subsystem::Abort() {
  admin_state_ = AdminState::kOffline;
  active_clients_.ClearAll();
  EnterState(OperState::kOffline, kNoClient);
}

void Subsystem::Restarting(uint32_t client_id, co::Coroutine *c) {
  oper_state_ = OperState::kRestarting;
  capcom_.SendSubsystemStatusEvent(this);
  if (AllProcessesStopped()) {
    if (parents_.empty()) {
      // We have no parents, restart now.
      RestartNow(client_id);
      return;
    }
    // Parents exist, notify them of the restart.
    NotifyParents();
  }

  // We still running processes.  Kill them and wait for them all to stop.
  // Stopping processes.
  StopProcesses(c);

  // Wait for all our processes to stop, then notify the parents that
  // we've stopped and are ready to be restarted.
  RunSubsystemInState(
      c, [ subsystem = shared_from_this(),
           client_id ](EventSource event_source,
                       std::shared_ptr<stagezero::Client> stagezero_client,
                       co::Coroutine * c)
             ->StateTransition {
               switch (event_source) {
               case EventSource::kStageZero: {
                 // Event from stagezero.
                 absl::StatusOr<std::shared_ptr<stagezero::control::Event>> e =
                     stagezero_client->ReadEvent(c);
                 if (!e.ok()) {
                   subsystem->capcom_.logger_.Log(
                       toolbelt::LogLevel::kError, "Failed to read event %s",
                       e.status().ToString().c_str());
                 }
                 std::shared_ptr<stagezero::control::Event> event = *e;
                 switch (event->event_case()) {
                 case stagezero::control::Event::kStart: {
                   // This might happen if a process crashed while others are
                   // starting up.  Ignore it, as it will be replaced by a kStop
                   // event when the process is killed.
                   break;
                 }
                 case stagezero::control::Event::kStop: {
                   Process *proc =
                       subsystem->FindProcess(event->stop().process_id());
                   if (proc != nullptr) {
                     proc->SetStopped();
                     subsystem->DeleteProcessId(proc->GetProcessId());
                   }
                   break;
                 }
                 case stagezero::control::Event::kOutput:
                   break;
                 case stagezero::control::Event::kLog:
                   subsystem->capcom_.Log(event->log());
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
                   subsystem->capcom_.logger_.Log(
                       toolbelt::LogLevel::kError, "%s",
                       message.status().ToString().c_str());
                 }
                 switch (message->code) {
                 case Message::kChangeAdmin:
                   if (message->state.admin == AdminState::kOnline) {
                     // We are restarting and have been asked to go online.
                     subsystem->RestartNow(client_id);
                     return StateTransition::kLeave;
                   }
                   subsystem->NotifyParents();
                   break;
                 case Message::kReportOper:
                   // Notification from a child while in restarting state.  We
                   // shouldn't get this.
                   subsystem->GetLogger().Log(
                       toolbelt::LogLevel::kError,
                       "Subsystem %s has reported oper state "
                       "change to %s while in restarting state",
                       message->sender->Name().c_str(),
                       OperStateName(message->state.oper));
                   break;
                 case Message::kAbort:
                   subsystem->Abort();
                   return StateTransition::kLeave;
                 }
                 break;
               }
               case EventSource::kUnknown:
                 break;
               }
               // If all our processes are not stopped we can notify our
               // parents.  We stay in this state.
               if (!subsystem->AllProcessesStopped()) {
                 return StateTransition::kStay;
               }

               // All our processes are down.  Notify the parents that we have
               // stopped everything and are in kRestarting state.
               if (subsystem->parents_.empty()) {
                 // We have no parents.  Restart now and leave this state.
                 subsystem->RestartNow(client_id);
                 return StateTransition::kLeave;
               }
               // Notify the parents and stay in kRestarting state.
               subsystem->NotifyParents();
               return StateTransition::kStay;
             });
}

void Subsystem::Broken(uint32_t client_id, co::Coroutine *c) {
  oper_state_ = OperState::kBroken;
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);
  RunSubsystemInState(
      c, [ subsystem = shared_from_this(),
           client_id ](EventSource event_source,
                       std::shared_ptr<stagezero::Client> stagezero_client,
                       co::Coroutine * c)
             ->StateTransition {
               if (event_source == EventSource::kMessage) {
                 // Incoming message.
                 absl::StatusOr<Message> message = subsystem->ReadMessage();
                 if (!message.ok()) {
                   subsystem->capcom_.logger_.Log(
                       toolbelt::LogLevel::kError, "%s",
                       message.status().ToString().c_str());
                 }
                 switch (message->code) {
                 case Message::kChangeAdmin:
                   subsystem->num_restarts_ = 0; // Reset restart counter.
                   if (message->state.admin == AdminState::kOnline) {
                     subsystem->active_clients_.Set(client_id);
                     subsystem->EnterState(OperState::kStartingChildren,
                                           client_id);
                   } else {
                     // Stop all children.
                     subsystem->active_clients_.Clear(client_id);
                     subsystem->EnterState(OperState::kStoppingChildren,
                                           client_id);
                   }
                   return StateTransition::kLeave;
                 case Message::kReportOper:
                   subsystem->GetLogger().Log(
                       toolbelt::LogLevel::kInfo,
                       "Subsystem %s has reported oper state "
                       "change to %s while it is broken",
                       message->sender->Name().c_str(),
                       OperStateName(message->state.oper));
                   break;
                 case Message::kAbort:
                   subsystem->Abort();
                   return StateTransition::kLeave;
                 }
               }
               return StateTransition::kStay;
             });
}

void Subsystem::RaiseAlarm(const Alarm &alarm) {
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

absl::Status Subsystem::AddStaticProcess(
    const stagezero::config::StaticProcess &proc,
    const stagezero::config::ProcessOptions &options,
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams,
    const Compute *compute, co::Coroutine *c) {
  if (proc.executable().empty()) {
    return absl::InternalError(absl::StrFormat(
        "Missing executable for static process %s", options.name()));
  }

  absl::StatusOr<std::shared_ptr<stagezero::Client>> client =
      ConnectToStageZero(compute, c);
  if (!client.ok()) {
    return client.status();
  }

  auto p = std::make_unique<StaticProcess>(
      capcom_, options.name(), proc.executable(), options, streams, *client);
  processes_.push_back(std::move(p));

  return absl::OkStatus();
}

absl::Status Subsystem::AddZygote(
    const stagezero::config::StaticProcess &proc,
    const stagezero::config::ProcessOptions &options,
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams,
    const Compute *compute, co::Coroutine *c) {
  if (proc.executable().empty()) {
    return absl::InternalError(
        absl::StrFormat("Missing executable for zygote %s", options.name()));
  }

  absl::StatusOr<std::shared_ptr<stagezero::Client>> client =
      ConnectToStageZero(compute, c);
  if (!client.ok()) {
    return client.status();
  }

  Zygote *z = FindZygote(options.name());
  if (z != nullptr) {
    return absl::InternalError(
        absl::StrFormat("Zygote %s already exists", options.name()));
  }

  auto p = std::make_unique<Zygote>(capcom_, options.name(), proc.executable(),
                                    options, streams, *client);
  capcom_.AddZygote(options.name(), p.get());

  processes_.push_back(std::move(p));
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
    const Compute *compute, co::Coroutine *c) {
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
  absl::StatusOr<std::shared_ptr<stagezero::Client>> client =
      ConnectToStageZero(compute, c);
  if (!client.ok()) {
    return client.status();
  }

  auto p = std::make_unique<VirtualProcess>(
      capcom_, options.name(), proc.zygote(), proc.dso(), proc.main_func(),
      options, streams, *client);
  processes_.push_back(std::move(p));

  return absl::OkStatus();
}

void Subsystem::BuildStatus(stagezero::proto::SubsystemStatus *status) {
  status->set_name(name_);

  switch (admin_state_) {
  case AdminState::kOffline:
    status->set_admin_state(stagezero::proto::ADMIN_OFFLINE);
    break;
  case AdminState::kOnline:
    status->set_admin_state(stagezero::proto::ADMIN_ONLINE);
    break;
  }

  switch (oper_state_) {
  case OperState::kOffline:
    status->set_oper_state(stagezero::proto::OPER_OFFLINE);
    break;
  case OperState::kStartingChildren:
    status->set_oper_state(stagezero::proto::OPER_STARTING_CHILDREN);
    break;
  case OperState::kStartingProcesses:
    status->set_oper_state(stagezero::proto::OPER_STARTING_PROCESSES);
    break;
  case OperState::kOnline:
    status->set_oper_state(stagezero::proto::OPER_ONLINE);
    break;
  case OperState::kStoppingChildren:
    status->set_oper_state(stagezero::proto::OPER_STOPPING_CHILDREN);
    break;
  case OperState::kStoppingProcesses:
    status->set_oper_state(stagezero::proto::OPER_STOPPING_PROCESSES);
    break;
  case OperState::kRestarting:
    status->set_oper_state(stagezero::proto::OPER_RESTARTING);
    break;
  case OperState::kBroken:
    status->set_oper_state(stagezero::proto::OPER_BROKEN);
    break;
  }

  // Processes.
  for (auto &proc : processes_) {
    auto *p = status->add_processes();
    p->set_name(proc->Name());
    p->set_process_id(proc->GetProcessId());
    p->set_pid(proc->GetPid());
    p->set_running(proc->IsRunning());
  }
}

void Subsystem::CollectAlarms(std::vector<Alarm *> &alarms) const {}

void Process::ParseOptions(const stagezero::config::ProcessOptions &options) {
  description_ = options.description();

  // Copy args.
  args_.resize(options.args_size());
  std::copy(options.args().begin(), options.args().end(), args_.begin());

  // Copy vars.
  for (auto &var : options.vars()) {
    vars_.push_back({var.name(), var.value(), var.exported()});
  }
  startup_timeout_secs_ = options.startup_timeout_secs();
  sigint_shutdown_timeout_secs_ = options.sigint_shutdown_timeout_secs();
  sigterm_shutdown_timeout_secs_ = options.sigterm_shutdown_timeout_secs();
  notify_ = options.notify();
}

void Process::ParseStreams(
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams) {
  for (auto &s : streams) {
    Stream stream;
    if (absl::Status status = stream.FromProto(s); !status.ok()) {
      capcom_.logger_.Log(toolbelt::LogLevel::kError,
                          "Failed to parse stream control: %s",
                          status.ToString().c_str());
      continue;
    }
    streams_.push_back(stream);
  }
}

absl::Status Process::Stop(co::Coroutine *c) {
  return client_->StopProcess(process_id_, c);
}

void Process::RaiseAlarm(Capcom &capcom, const Alarm &alarm) {
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

StaticProcess::StaticProcess(
    Capcom &capcom, std::string name, std::string executable,
    const stagezero::config::ProcessOptions &options,
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams,
    std::shared_ptr<stagezero::Client> client)
    : Process(capcom, std::move(name), std::move(client)),
      executable_(std::move(executable)) {
  ParseOptions(options);
  ParseStreams(streams);
}

absl::Status StaticProcess::Launch(co::Coroutine *c) {
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

  options.streams = streams_;

  std::cerr << "trying to launch " << Name() << std::endl;
  absl::StatusOr<std::pair<std::string, int>> s =
      client_->LaunchStaticProcess(Name(), executable_, options, c);
  if (!s.ok()) {
    std::cerr << s.status().ToString() << std::endl;
    return s.status();
  }
  process_id_ = s->first;
  pid_ = s->second;
  return absl::OkStatus();
}

absl::Status Zygote::Launch(co::Coroutine *c) {
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

  options.streams = streams_;

  absl::StatusOr<std::pair<std::string, int>> s =
      client_->LaunchZygote(Name(), executable_, options, c);
  if (!s.ok()) {
    return s.status();
  }
  process_id_ = s->first;
  pid_ = s->second;
  return absl::OkStatus();
}

VirtualProcess::VirtualProcess(
    Capcom &capcom, std::string name, std::string zygote_name, std::string dso,
    std::string main_func, const stagezero::config::ProcessOptions &options,
    const google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl>
        &streams,
    std::shared_ptr<stagezero::Client> client)
    : Process(capcom, name, std::move(client)), zygote_name_(zygote_name),
      dso_(dso), main_func_(main_func) {
  ParseOptions(options);
  ParseStreams(streams);
}

absl::Status VirtualProcess::Launch(co::Coroutine *c) {
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

  options.streams = streams_;

  absl::StatusOr<std::pair<std::string, int>> s = client_->LaunchVirtualProcess(
      Name(), zygote_name_, dso_, main_func_, options, c);
  if (!s.ok()) {
    return s.status();
  }
  process_id_ = s->first;
  pid_ = s->second;
  return absl::OkStatus();
}

} // namespace stagezero::capcom
