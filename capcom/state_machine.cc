// All Rights Reserved
// See LICENSE file for licensing information.

#include "capcom/capcom.h"
#include "capcom/subsystem.h"

#include <unistd.h>

namespace adastra::capcom {

// NOTE: Keep this array up to date with OperState.  If you add a new
// operational state, make sure to add something to this array, in the
// correct position.
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
  capcom_.Log(Name(), toolbelt::LogLevel::kDebug,
              "Subsystem %s entering %s from %s", Name().c_str(),
              OperStateName(state), OperStateName(oper_state_));
  prev_oper_state_ = oper_state_;
  oper_state_ = state;
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
  fds.push_back({subsystem->message_pipe_.ReadFd().Fd(), POLLIN});

  while (subsystem->running_) {
    int fd = c->Wait(fds);
    if (fd == subsystem->interrupt_.GetPollFd().Fd()) {
      // Interrupt.
      subsystem->interrupt_.Clear();
      continue;
    }
    EventSource event_source = EventSource::kUnknown;
    std::shared_ptr<stagezero::Client> found_client;
    if (fd == subsystem->message_pipe_.ReadFd().Fd()) {
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
      subsystem->capcom_.Log(subsystem->Name(), toolbelt::LogLevel::kError,
                             "Event from unknown source in subsystem %s",
                             subsystem->Name().c_str());
    }
  }
}

OperState
Subsystem::HandleAdminCommand(const Message &message,
                              OperState next_state_active_clients,
                              OperState next_state_no_active_clients) {
  auto subsystem = shared_from_this();
  capcom_.Log(subsystem->Name(), toolbelt::LogLevel::kDebug,
              "Subsystem %s is in admin state %s/%s and got a command "
              "to change admin state to %s from client %d",
              Name().c_str(), AdminStateName(subsystem->admin_state_),
              OperStateName(message.state.oper),
              AdminStateName(message.state.admin), message.client_id);

  subsystem->SendToChildren(message.state.admin, message.client_id);

  if (message.state.admin == AdminState::kOnline) {
    if (message.client_id != kNoClient) {
      subsystem->active_clients_.Set(message.client_id);
    }
  } else {
    if (message.client_id != kNoClient) {
      subsystem->active_clients_.Clear(message.client_id);
    }
  }

  OperState next_state;
  if (subsystem->active_clients_.IsEmpty()) {
    next_state = next_state_no_active_clients;
  } else {
    next_state = next_state_active_clients;
  }
  if (next_state == subsystem->oper_state_) {
    // We are not changing admin states, but the parent will be expecting
    // to see an event.
    subsystem->capcom_.SendSubsystemStatusEvent(subsystem.get());
    subsystem->NotifyParents();
  }
  return next_state;
}

void Subsystem::Offline(uint32_t client_id, co::Coroutine *c) {
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);
  restart_count_ = 0;
  alarm_count_ = 0;

  for (auto& proc : processes_) {
    proc->ResetAlarmCount();
  }
  
  // Only log an info message if the state has changed.  Seeing "Foo is OFFLINE"
  // at startup isn't helpful as an info message, but there is a debug message
  // that shows it.
  if (prev_oper_state_ != oper_state_) {
    capcom_.Log(Name(), toolbelt::LogLevel::kInfo,
                "Subsystem %s has gone OFFLINE", Name().c_str());
  }
  OperState next_state = OperState::kOffline;
  RunSubsystemInState(
      c, [ subsystem = shared_from_this(),
           &next_state ](EventSource event_source,
                         std::shared_ptr<stagezero::Client> stagezero_client,
                         co::Coroutine * c)
             ->StateTransition {
               switch (event_source) {
               case EventSource::kStageZero: {
                 // Event from stagezero.
                 absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
                     stagezero_client->ReadEvent(c);
                 if (!e.ok()) {
                   subsystem->capcom_.Log(subsystem->Name(),
                                          toolbelt::LogLevel::kError,
                                          "Failed to read event %s",
                                          e.status().ToString().c_str());
                 }
                 std::shared_ptr<adastra::stagezero::control::Event> event = *e;
                 switch (event->event_case()) {
                 case adastra::stagezero::control::Event::kStart: {
                   break;
                 }
                 case adastra::stagezero::control::Event::kStop:
                   break;
                 case adastra::stagezero::control::Event::kOutput:
                   subsystem->SendOutput(event->output().fd(),
                                         event->output().data(), c);
                   break;
                 case adastra::stagezero::control::Event::kLog:
                   subsystem->capcom_.Log(event->log());
                   break;
                 case adastra::stagezero::control::Event::EVENT_NOT_SET:
                   break;
                 }
                 break;
               }

               case EventSource::kMessage: {
                 // Incoming message.
                 absl::StatusOr<Message> message = subsystem->ReadMessage();
                 if (!message.ok()) {
                   subsystem->capcom_.Log(subsystem->Name(),
                                          toolbelt::LogLevel::kError, "%s",
                                          message.status().ToString().c_str());
                 }
                 switch (message->code) {
                 case Message::kChangeAdmin:
                   next_state = subsystem->HandleAdminCommand(
                       *message, OperState::kStartingChildren,
                       OperState::kOffline);
                   if (next_state == OperState::kStartingChildren) {
                     subsystem->admin_state_ = AdminState::kOnline;
                   }
                   subsystem->interactive_ = message->interactive;
                   if (message->interactive) {
                     subsystem->interactive_output_.SetFd(message->output_fd);
                     subsystem->interactive_terminal_.rows = message->rows;
                     subsystem->interactive_terminal_.cols = message->cols;
                     subsystem->interactive_terminal_.name =
                         std::string(message->term_name);
                   } else {
                     subsystem->interactive_output_.Close();
                   }

                   break;

                 case Message::kReportOper:
                   subsystem->capcom_.Log(
                       subsystem->Name(), toolbelt::LogLevel::kDebug,
                       "Subsystem %s has reported oper state as %s",
                       message->sender->Name().c_str(),
                       OperStateName(message->state.oper));
                   subsystem->NotifyParents();
                   break;
                 case Message::kAbort:
                   break;
                 }
                 break;
               }
               case EventSource::kUnknown:
                 break;
               }

               if (next_state != OperState::kOffline) {
                 subsystem->EnterState(next_state, kNoClient);
                 return StateTransition::kLeave;
               }
               return StateTransition::kStay;
             });
}

void Subsystem::StartingChildren(uint32_t client_id, co::Coroutine *c) {
  if (children_.empty()) {
    EnterState(OperState::kStartingProcesses, client_id);
    return;
  }
  SendToChildren(AdminState::kOnline, client_id);
  NotifyParents();

  capcom_.SendSubsystemStatusEvent(this);

  // Mapping to hold whether child has notified.
  absl::flat_hash_map<Subsystem *, bool> child_notified;
  for (auto &child : children_) {
    child_notified.insert(std::make_pair(child.get(), false));
  }

  OperState next_state = OperState::kStartingProcesses;
  RunSubsystemInState(
      c,
      [
        subsystem = shared_from_this(), &client_id, &child_notified, &next_state
      ](EventSource event_source,
        std::shared_ptr<stagezero::Client> stagezero_client, co::Coroutine * c)
          ->StateTransition {
            switch (event_source) {
            case EventSource::kStageZero: {
              // Event from stagezero.
              absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
                  stagezero_client->ReadEvent(c);
              if (!e.ok()) {
                subsystem->capcom_.Log(
                    subsystem->Name(), toolbelt::LogLevel::kError,
                    "Failed to read event %s", e.status().ToString().c_str());
              }
              std::shared_ptr<adastra::stagezero::control::Event> event = *e;

              switch (event->event_case()) {
              case adastra::stagezero::control::Event::kStart: {
                break;
              }
              case adastra::stagezero::control::Event::kStop:
                // One of our processes crashed while starting the children.
                // Since nothing should be running this is a late message.
                // Igore it.
                return StateTransition::kStay;

              case adastra::stagezero::control::Event::kOutput:
                subsystem->SendOutput(event->output().fd(),
                                      event->output().data(), c);
                break;
              case adastra::stagezero::control::Event::kLog:
                subsystem->capcom_.Log(event->log());
                break;
              case adastra::stagezero::control::Event::EVENT_NOT_SET:
                break;
              }
              break;
            }
            case EventSource::kMessage: {
              // Incoming message.
              absl::StatusOr<Message> message = subsystem->ReadMessage();
              if (!message.ok()) {
                subsystem->capcom_.Log(subsystem->Name(),
                                       toolbelt::LogLevel::kError, "%s",
                                       message.status().ToString().c_str());
              }
              switch (message->code) {
              case Message::kChangeAdmin:
                next_state = subsystem->HandleAdminCommand(
                    *message, OperState::kStartingProcesses,
                    OperState::kStoppingChildren);
                client_id = message->client_id;
                if (next_state == OperState::kStoppingChildren) {
                  subsystem->admin_state_ = AdminState::kOffline;
                }
                break;
              case Message::kReportOper:
                subsystem->capcom_.Log(
                    subsystem->Name(), toolbelt::LogLevel::kDebug,
                    "Subsystem %s has reported oper state as %s",
                    message->sender->Name().c_str(),
                    OperStateName(message->state.oper));
                if (message->state.oper == OperState::kOnline) {
                  child_notified[message->sender] = true;
                }
                subsystem->NotifyParents();
                break;
              case Message::kAbort:
                return subsystem->Abort(message->emergency_abort);
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

            if (next_state != OperState::kStartingProcesses) {
              subsystem->EnterState(next_state, client_id);
              return StateTransition::kLeave;
            }

            // We only start when the children are online.
            for (auto &child : subsystem->children_) {
              if (child->oper_state_ != OperState::kOnline) {
                return StateTransition::kStay;
              }
            }

            subsystem->EnterState(next_state, client_id);
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
    capcom_.Log(Name(), toolbelt::LogLevel::kError, "%s",
                status.ToString().c_str());
    RestartIfPossible(client_id, c);
    return;
  }
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);

  OperState next_state = OperState::kOnline;
  RunSubsystemInState(
      c, [ subsystem = shared_from_this(), &client_id,
           &next_state ](EventSource event_source,
                         std::shared_ptr<stagezero::Client> stagezero_client,
                         co::Coroutine * c)
             ->StateTransition {
               switch (event_source) {
               case EventSource::kStageZero: {
                 // Event from stagezero.
                 absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
                     stagezero_client->ReadEvent(c);
                 if (!e.ok()) {
                   subsystem->capcom_.Log(subsystem->Name(),
                                          toolbelt::LogLevel::kError,
                                          "Failed to read event %s",
                                          e.status().ToString().c_str());
                 }
                 std::shared_ptr<adastra::stagezero::control::Event> event = *e;
                 switch (event->event_case()) {
                 case adastra::stagezero::control::Event::kStart: {
                   Process *proc =
                       subsystem->FindProcess(event->start().process_id());
                   if (proc != nullptr) {
                     proc->SetRunning();
                     proc->ClearAlarm(subsystem->capcom_);
                   }
                   break;
                 }
                 case adastra::stagezero::control::Event::kStop: {
                   // Process failed to start.
                   if (!subsystem->capcom_.IsEmergencyAborting()) {
                     const stagezero::control::StopEvent &stop = event->stop();
                     Process *proc = subsystem->FindProcess(stop.process_id());
                     if (proc == nullptr) {
                       subsystem->capcom_.Log(
                           subsystem->Name(), toolbelt::LogLevel::kError,
                           "Can't find process", stop.process_id().c_str());
                       return StateTransition::kStay;
                     }
                     if (!proc->IsOneShot()) {
                       subsystem->capcom_.Log(
                           subsystem->Name(), toolbelt::LogLevel::kError,
                           "Process %s has crashed", stop.process_id().c_str());
                     }
                     int signal_or_status = stop.sig_or_status();
                     bool exited = stop.reason() != stagezero::control::StopEvent::SIGNAL;
                     return subsystem->RestartIfPossibleAfterProcessCrash(
                         stop.process_id(), client_id, exited, signal_or_status,
                         c);
                   }
                   return StateTransition::kStay;
                 }
                 case adastra::stagezero::control::Event::kOutput:
                   subsystem->SendOutput(event->output().fd(),
                                         event->output().data(), c);
                   break;
                 case adastra::stagezero::control::Event::kLog:
                   subsystem->capcom_.Log(event->log());
                   break;
                 case adastra::stagezero::control::Event::EVENT_NOT_SET:
                   break;
                 }
                 break;
               }
               case EventSource::kMessage: {
                 // Incoming message.
                 absl::StatusOr<Message> message = subsystem->ReadMessage();
                 if (!message.ok()) {
                   subsystem->capcom_.Log(subsystem->Name(),
                                          toolbelt::LogLevel::kError, "%s",
                                          message.status().ToString().c_str());
                 }
                 switch (message->code) {
                 case Message::kChangeAdmin:
                   next_state = subsystem->HandleAdminCommand(
                       *message, OperState::kOnline,
                       OperState::kStoppingProcesses);
                   client_id = message->client_id;
                   if (next_state == OperState::kStoppingProcesses) {
                     subsystem->admin_state_ = AdminState::kOffline;
                   }
                   break;
                 case Message::kReportOper:
                   subsystem->capcom_.Log(
                       subsystem->Name(), toolbelt::LogLevel::kDebug,
                       "Subsystem %s has reported oper state as %s",
                       message->sender->Name().c_str(),
                       OperStateName(message->state.oper));
                   subsystem->NotifyParents();
                   break;
                 case Message::kAbort:
                   return subsystem->Abort(message->emergency_abort);
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

               subsystem->EnterState(next_state, client_id);
               return StateTransition::kLeave;
             });
}

void Subsystem::Online(uint32_t client_id, co::Coroutine *c) {
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);

  capcom_.Log(Name(), toolbelt::LogLevel::kInfo, "Subsystem %s is now ONLINE",
              Name().c_str());
  OperState next_state = OperState::kOnline;
  RunSubsystemInState(
      c, [ subsystem = shared_from_this(), &client_id,
           &next_state ](EventSource event_source,
                         std::shared_ptr<stagezero::Client> stagezero_client,
                         co::Coroutine * c)
             ->StateTransition {
               switch (event_source) {
               case EventSource::kStageZero: {
                 // Event from stagezero.
                 absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
                     stagezero_client->ReadEvent(c);
                 if (!e.ok()) {
                   subsystem->capcom_.Log(subsystem->Name(),
                                          toolbelt::LogLevel::kError,
                                          "Failed to read event %s",
                                          e.status().ToString().c_str());
                 }
                 std::shared_ptr<adastra::stagezero::control::Event> event = *e;
                 switch (event->event_case()) {
                 case adastra::stagezero::control::Event::kStart: {
                   // We aren't going to get these as all processes are
                   // running.
                   break;
                 }
                 case adastra::stagezero::control::Event::kStop: {
                   if (subsystem->interactive_) {
                     Process *proc =
                         subsystem->FindProcess(event->stop().process_id());
                     if (proc != nullptr) {
                       subsystem->capcom_.Log(subsystem->Name(),
                                              toolbelt::LogLevel::kDebug,
                                              "Interactive process %s stopped",
                                              proc->Name().c_str());
                       proc->SetStopped();
                       subsystem->DeleteProcessId(proc->GetProcessId());
                       subsystem->interactive_output_.Reset();

                       // When an interacive process goes offline, its subsystem
                       // also goes offline, both admin and oper.
                       subsystem->admin_state_ = AdminState::kOffline;
                       subsystem->EnterState(OperState::kOffline, kNoClient);
                       return StateTransition::kLeave;
                     }
                   }

                   // Non-interative process crashed.  Restart.
                   if (!subsystem->capcom_.IsEmergencyAborting()) {
                     const stagezero::control::StopEvent &stop = event->stop();
                     Process *proc = subsystem->FindProcess(stop.process_id());
                     if (proc == nullptr) {
                       subsystem->capcom_.Log(
                           subsystem->Name(), toolbelt::LogLevel::kError,
                           "Can't find process", stop.process_id().c_str());
                       return StateTransition::kStay;
                     }
                     if (!proc->IsOneShot()) {
                       subsystem->capcom_.Log(
                           subsystem->Name(), toolbelt::LogLevel::kError,
                           "Process %s has crashed", stop.process_id().c_str());
                     }
                     int signal_or_status = stop.sig_or_status();
                     bool exited = stop.reason() != stagezero::control::StopEvent::SIGNAL;
                     return subsystem->RestartIfPossibleAfterProcessCrash(
                         stop.process_id(), client_id, exited, signal_or_status,
                         c);
                   }
                   return StateTransition::kStay;
                 }
                 case adastra::stagezero::control::Event::kOutput:
                   subsystem->SendOutput(event->output().fd(),
                                         event->output().data(), c);
                   break;
                 case adastra::stagezero::control::Event::kLog:
                   subsystem->capcom_.Log(event->log());
                   break;
                 case adastra::stagezero::control::Event::EVENT_NOT_SET:
                   break;
                 }
                 break;
               }
               case EventSource::kMessage: {
                 // Incoming message.
                 absl::StatusOr<Message> message = subsystem->ReadMessage();
                 if (!message.ok()) {
                   subsystem->capcom_.Log(subsystem->Name(),
                                          toolbelt::LogLevel::kError, "%s",
                                          message.status().ToString().c_str());
                 }
                 switch (message->code) {
                 case Message::kChangeAdmin:
                   next_state = subsystem->HandleAdminCommand(
                       *message, OperState::kOnline,
                       OperState::kStoppingProcesses);
                   client_id = message->client_id;
                   if (next_state == OperState::kStoppingProcesses) {
                     subsystem->admin_state_ = AdminState::kOffline;
                   }
                   break;
                 case Message::kReportOper:
                   subsystem->capcom_.Log(
                       subsystem->Name(), toolbelt::LogLevel::kDebug,
                       "Subsystem %s has reported oper state as %s",
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
                   return subsystem->Abort(message->emergency_abort);
                 }
                 break;
               }
               case EventSource::kUnknown:
                 break;
               }

               if (next_state != OperState::kOnline) {
                 subsystem->EnterState(next_state, client_id);
                 return StateTransition::kLeave;
               }
               return StateTransition::kStay;
             });
}

void Subsystem::StoppingProcesses(uint32_t client_id, co::Coroutine *c) {
  int num_running_processes = 0;
  for (auto &proc : processes_) {
    if (proc->IsRunning()) {
      num_running_processes++;
    }
  }
  if (num_running_processes == 0) {
    EnterState(OperState::kStoppingChildren, client_id);
    return;
  }
  StopProcesses(c);
  NotifyParents();
  capcom_.SendSubsystemStatusEvent(this);

  OperState next_state = OperState::kStoppingChildren;
  RunSubsystemInState(
      c, [ subsystem = shared_from_this(), client_id,
           &next_state ](EventSource event_source,
                         std::shared_ptr<stagezero::Client> stagezero_client,
                         co::Coroutine * c)
             ->StateTransition {
               switch (event_source) {
               case EventSource::kStageZero: {
                 // Event from stagezero.
                 absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
                     stagezero_client->ReadEvent(c);
                 if (!e.ok()) {
                   subsystem->capcom_.Log(subsystem->Name(),
                                          toolbelt::LogLevel::kError,
                                          "Failed to read event %s",
                                          e.status().ToString().c_str());
                 }
                 std::shared_ptr<adastra::stagezero::control::Event> event = *e;
                 switch (event->event_case()) {
                 case adastra::stagezero::control::Event::kStart: {
                   // We aren't going to get these as all processes are
                   // stopping.
                   break;
                 }
                 case adastra::stagezero::control::Event::kStop: {
                   // Process stopped OK.
                   Process *proc =
                       subsystem->FindProcess(event->stop().process_id());
                   if (proc != nullptr) {
                     subsystem->capcom_.Log(
                         subsystem->Name(), toolbelt::LogLevel::kDebug,
                         "Process %s stopped", proc->Name().c_str());
                     proc->SetStopped();
                     subsystem->DeleteProcessId(proc->GetProcessId());
                     if (subsystem->interactive_) {
                       subsystem->interactive_output_.Reset();
                     }
                   }
                   break;
                 }
                 case adastra::stagezero::control::Event::kOutput:
                   subsystem->SendOutput(event->output().fd(),
                                         event->output().data(), c);
                   break;
                 case adastra::stagezero::control::Event::kLog:
                   subsystem->capcom_.Log(event->log());
                   break;
                 case adastra::stagezero::control::Event::EVENT_NOT_SET:
                   break;
                 }
                 break;
               }
               case EventSource::kMessage: {
                 // Incoming message.
                 absl::StatusOr<Message> message = subsystem->ReadMessage();
                 if (!message.ok()) {
                   subsystem->capcom_.Log(subsystem->Name(),
                                          toolbelt::LogLevel::kError, "%s",
                                          message.status().ToString().c_str());
                 }
                 switch (message->code) {
                 case Message::kChangeAdmin:
                   next_state = subsystem->HandleAdminCommand(
                       *message, OperState::kStartingProcesses,
                       OperState::kStoppingChildren);
                   break;
                 case Message::kReportOper:
                   subsystem->capcom_.Log(
                       subsystem->Name(), toolbelt::LogLevel::kDebug,
                       "Subsystem %s has reported oper state as %s",
                       message->sender->Name().c_str(),
                       OperStateName(message->state.oper));
                   subsystem->NotifyParents();
                   break;
                 case Message::kAbort:
                   return subsystem->Abort(message->emergency_abort);
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
               subsystem->EnterState(next_state, client_id);
               return StateTransition::kLeave;
             });
}

void Subsystem::StoppingChildren(uint32_t client_id, co::Coroutine *c) {
  if (children_.empty()) {
    EnterState(OperState::kOffline, client_id);
    return;
  }
  SendToChildren(AdminState::kOffline, client_id);
  NotifyParents();

  capcom_.SendSubsystemStatusEvent(this);

  // Mapping to hold whether child has notified.
  absl::flat_hash_map<Subsystem *, bool> child_notified;
  for (auto &child : children_) {
    child_notified.insert(std::make_pair(child.get(), false));
  }

  absl::flat_hash_set<uint32_t> offline_requests;
  offline_requests.insert(client_id);

  OperState next_state = OperState::kOffline;
  RunSubsystemInState(
      c,
      [
        subsystem = shared_from_this(), client_id, &child_notified, &next_state,
        &offline_requests
      ](EventSource event_source,
        std::shared_ptr<stagezero::Client> stagezero_client, co::Coroutine * c)
          ->StateTransition {
            switch (event_source) {
            case EventSource::kStageZero: {
              // Event from stagezero.
              absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
                  stagezero_client->ReadEvent(c);
              if (!e.ok()) {
                subsystem->capcom_.Log(
                    subsystem->Name(), toolbelt::LogLevel::kError,
                    "Failed to read event %s", e.status().ToString().c_str());
              }
              std::shared_ptr<adastra::stagezero::control::Event> event = *e;
              switch (event->event_case()) {
              case adastra::stagezero::control::Event::kStart: {
                // We shouldn't get this because all our processes are
                // stopped.
                break;
              }
              case adastra::stagezero::control::Event::kStop: {
                // We shouldn't get this because all our processes are
                // stopped.
                break;
              }
              case adastra::stagezero::control::Event::kOutput:
                subsystem->SendOutput(event->output().fd(),
                                      event->output().data(), c);
                break;
              case adastra::stagezero::control::Event::kLog:
                subsystem->capcom_.Log(event->log());
                break;
              case adastra::stagezero::control::Event::EVENT_NOT_SET:
                break;
              }
              break;
            }
            case EventSource::kMessage: {
              // Incoming message.
              absl::StatusOr<Message> message = subsystem->ReadMessage();
              if (!message.ok()) {
                subsystem->capcom_.Log(subsystem->Name(),
                                       toolbelt::LogLevel::kError, "%s",
                                       message.status().ToString().c_str());
              }
              switch (message->code) {
              case Message::kChangeAdmin:
                next_state = subsystem->HandleAdminCommand(
                    *message, OperState::kStartingChildren,
                    OperState::kOffline);

                // We are trying to stop the children.  They may or may not
                // go offline depending on the other active clients that
                // need themm.  We need to keep track of all the clients
                // that have requested that we go offline and only go
                // offline when all of them have been removed from the
                // active client list in the children.
                if (message->state.admin == AdminState::kOffline) {
                  offline_requests.insert(message->client_id);
                } else {
                  offline_requests.erase(message->client_id);
                }
                break;
              case Message::kReportOper:
                subsystem->capcom_.Log(
                    subsystem->Name(), toolbelt::LogLevel::kDebug,
                    "Subsystem %s has reported oper state as %s",
                    message->sender->Name().c_str(),
                    OperStateName(message->state.oper));

                // The children may be going offline or staying online
                // depending on their active clients.  Record when we get an
                // event saying that they are in this state.
                if (message->state.oper == OperState::kOnline ||
                    message->state.oper == OperState::kOffline) {
                  child_notified[message->sender] = true;
                }
                subsystem->NotifyParents();
                break;
              case Message::kAbort:
                return subsystem->Abort(message->emergency_abort);
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

            if (next_state == OperState::kStartingChildren) {
              subsystem->EnterState(next_state, client_id);
              return StateTransition::kLeave;
            }

            // We are going offline, but our children might stay online
            // if they are needed by other subsystems.  We can't check
            // for their oper state, but we can check if they still contain
            // any of the client ids requested to go offline.

            for (auto &child : subsystem->children_) {
              for (auto &id : offline_requests) {
                if (child->active_clients_.Contains(id)) {
                  return StateTransition::kStay;
                }
              }
            }
            subsystem->EnterState(next_state, client_id);
            return StateTransition::kLeave;
          });
}

void Subsystem::RestartNow(uint32_t client_id) {
  EnterState(OperState::kStartingChildren, client_id);
}

Subsystem::StateTransition Subsystem::RestartIfPossibleAfterProcessCrash(
    std::string process_id, uint32_t client_id, bool exited,
    int signal_or_status, co::Coroutine *c) {

  if (capcom_.IsEmergencyAborting()) {
    return StateTransition::kStay;
  }

  Process *proc = FindProcess(process_id);
  if (proc == nullptr) {
    capcom_.Log(Name(), toolbelt::LogLevel::kError,
                "Cannot find process %s for restart", process_id.c_str());
    return StateTransition::kStay;
  }
  if (IsCritical()) {
    // A critical subsystem has had a process crash.  We need to bring the
    // whole system down.
    capcom_.AddCoroutine(std::make_unique<co::Coroutine>(
        capcom_.co_scheduler_, [this, proc](co::Coroutine *c2) {
          absl::Status status = capcom_.Abort(
              absl::StrFormat("Process %s of critical subsystem %s has "
                              "suffered a failure and we need to shut down",
                              proc->Name(), Name()),
              /*emergency=*/true, c2);
          if (!status.ok()) {
            capcom_.logger_.Log(
                toolbelt::LogLevel::kFatal,
                "Failed to abort cleanly, just shutting down: %s",
                status.ToString().c_str());
          }
        }));
    return StateTransition::kStay; // Doesn't matter.
  }
  proc->SetStopped();
  DeleteProcessId(proc->GetProcessId());

  if (proc->IsOneShot()) {
    // A oneshot process has exited.  If it exited with
    if (exited) {
      if (signal_or_status == 0) {
        // All good, oneshot terminated with success.
        return StateTransition::kStay;
      }
    }
    // Oneshot terminated with a signal or non-zero exit status.  This is deemed
    // to be a failure of the process.  Since we can't restart oneshot processes
    // (they are meant to run once only), this subsystem is now broken.
    EnterState(OperState::kBroken, client_id);

    std::string reason;
    if (exited) {
      reason = absl::StrFormat("exited with status %d", signal_or_status);
    } else {
      reason = absl::StrFormat("received signal %d \"%s\"", signal_or_status,
                               strsignal(signal_or_status));
    }
    if (!capcom_.TestMode()) {
      proc->RaiseAlarm(capcom_,
                       {.name = proc->Name(),
                        .type = Alarm::Type::kProcess,
                        .severity = Alarm::Severity::kCritical,
                        .reason = Alarm::Reason::kCrashed,
                        .status = Alarm::Status::kRaised,
                        .details = absl::StrFormat(
                            "Oneshot process %s %s, subsystem %s is broken",
                            proc->Name(), reason, Name())});
      return StateTransition::kLeave;
    }
  }

  // If we are test mode don't restart anything and perform an emergency
  // abort.
  if (capcom_.TestMode()) {
    capcom_.logger_.Log(toolbelt::LogLevel::kError, "Test mode, shutting down");
    capcom_.AddCoroutine(std::make_unique<
                         co::Coroutine>(capcom_.co_scheduler_, [this, proc](
                                                                   co::Coroutine
                                                                       *c2) {
      absl::Status status = capcom_.Abort(
          absl::StrFormat(
              "Process %s of subsystem %s has "
              "suffered a failure and we are in test mode.  Shutting down now.",
              proc->Name(), Name()),
          /*emergency=*/true, c2);
      if (!status.ok()) {
        capcom_.logger_.Log(toolbelt::LogLevel::kFatal,
                            "Failed to abort cleanly, just shutting down: %s",
                            status.ToString().c_str());
      }
    }));

    return StateTransition::kStay; // Doesn't matter.
  }

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
    return StateTransition::kLeave;
  }

  proc->RaiseAlarm(capcom_, {.name = proc->Name(),
                             .type = Alarm::Type::kProcess,
                             .severity = Alarm::Severity::kWarning,
                             .reason = Alarm::Reason::kCrashed,
                             .status = Alarm::Status::kRaised,
                             .details = absl::StrFormat("Process %s is being restarted",
                                                        proc->Name())});

  // Delay before restarting.
  restart_delay_ = std::min(kMaxRestartDelay, restart_delay_ * 2);
  ++num_restarts_;
  c->Sleep(restart_delay_.count());
  EnterState(OperState::kRestarting, client_id);
  return StateTransition::kLeave;
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

Subsystem::StateTransition Subsystem::Abort(bool emergency) {
  if (!emergency && IsCritical()) {
    return StateTransition::kStay;
  }
  admin_state_ = AdminState::kOffline;
  active_clients_.ClearAll();
  for (auto &proc : processes_) {
    proc->SetStopped();
  }
  EnterState(OperState::kOffline, kNoClient);
  return StateTransition::kLeave;
}

void Subsystem::Restarting(uint32_t client_id, co::Coroutine *c) {
  restart_count_++;
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
                 absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
                     stagezero_client->ReadEvent(c);
                 if (!e.ok()) {
                   subsystem->capcom_.Log(subsystem->Name(),
                                          toolbelt::LogLevel::kError,
                                          "Failed to read event %s",
                                          e.status().ToString().c_str());
                 }
                 std::shared_ptr<adastra::stagezero::control::Event> event = *e;
                 switch (event->event_case()) {
                 case adastra::stagezero::control::Event::kStart: {
                   // This might happen if a process crashed while others are
                   // starting up.  Ignore it, as it will be replaced by a
                   // kStop event when the process is killed.
                   break;
                 }
                 case adastra::stagezero::control::Event::kStop: {
                   Process *proc =
                       subsystem->FindProcess(event->stop().process_id());
                   if (proc != nullptr) {
                     proc->SetStopped();
                     subsystem->DeleteProcessId(proc->GetProcessId());
                   }
                   break;
                 }
                 case adastra::stagezero::control::Event::kOutput:
                   subsystem->SendOutput(event->output().fd(),
                                         event->output().data(), c);
                   break;
                 case adastra::stagezero::control::Event::kLog:
                   subsystem->capcom_.Log(event->log());
                   break;
                 case adastra::stagezero::control::Event::EVENT_NOT_SET:
                   break;
                 }
                 break;
               }
               case EventSource::kMessage: {
                 // Incoming message.
                 absl::StatusOr<Message> message = subsystem->ReadMessage();
                 if (!message.ok()) {
                   subsystem->capcom_.Log(subsystem->Name(),
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
                   subsystem->capcom_.Log(
                       subsystem->Name(), toolbelt::LogLevel::kError,
                       "Subsystem %s has reported oper state "
                       "as %s while in restarting state",
                       message->sender->Name().c_str(),
                       OperStateName(message->state.oper));
                   break;
                 case Message::kAbort:
                   return subsystem->Abort(message->emergency_abort);
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
                   subsystem->capcom_.Log(subsystem->Name(),
                                          toolbelt::LogLevel::kError, "%s",
                                          message.status().ToString().c_str());
                 }
                 switch (message->code) {
                 case Message::kChangeAdmin:
                   subsystem->num_restarts_ = 0; // Reset restart counter.
                   subsystem->restart_count_ = 0;
                   if (message->state.admin == AdminState::kOnline) {
                     subsystem->active_clients_.Set(client_id);
                     subsystem->EnterState(OperState::kStartingChildren,
                                           client_id);
                   } else {
                     // Stop all children.
                     subsystem->active_clients_.Clear(client_id);
                     subsystem->admin_state_ = AdminState::kOffline;
                     subsystem->EnterState(OperState::kStoppingChildren,
                                           client_id);
                   }
                   return StateTransition::kLeave;
                 case Message::kReportOper:
                   subsystem->capcom_.Log(
                       subsystem->Name(), toolbelt::LogLevel::kInfo,
                       "Subsystem %s has reported oper state "
                       "as %s while it is broken",
                       message->sender->Name().c_str(),
                       OperStateName(message->state.oper));
                   break;
                 case Message::kAbort:
                   return subsystem->Abort(message->emergency_abort);
                 }
               }
               return StateTransition::kStay;
             });
}

} // namespace adastra::capcom
