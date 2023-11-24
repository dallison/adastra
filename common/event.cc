// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "common/event.h"
#include "absl/strings/str_format.h"
#include "proto/event.pb.h"

namespace stagezero {

void Event::ToProto(proto::Event *dest) const {
  switch (type) {
  case EventType::kSubsystemStatus: {
    SubsystemStatusEvent status = std::get<0>(event);
    auto s = dest->mutable_subsystem_status();
    s->set_name(status.subsystem);
    switch (status.admin_state) {
    case AdminState::kOffline:
      s->set_admin_state(stagezero::proto::ADMIN_OFFLINE);
      break;
    case AdminState::kOnline:
      s->set_admin_state(stagezero::proto::ADMIN_ONLINE);
      break;
    }
    switch (status.oper_state) {
    case OperState::kBroken:
      s->set_oper_state(stagezero::proto::OPER_BROKEN);
      break;
    case OperState::kOffline:
      s->set_oper_state(stagezero::proto::OPER_OFFLINE);
      break;
    case OperState::kOnline:
      s->set_oper_state(stagezero::proto::OPER_ONLINE);
      break;
    case OperState::kRestarting:
      s->set_oper_state(stagezero::proto::OPER_RESTARTING);
      break;
    case OperState::kStartingChildren:
      s->set_oper_state(stagezero::proto::OPER_STARTING_CHILDREN);
      break;
    case OperState::kStartingProcesses:
      s->set_oper_state(stagezero::proto::OPER_STARTING_PROCESSES);
      break;
    case OperState::kStoppingChildren:
      s->set_oper_state(stagezero::proto::OPER_STOPPING_CHILDREN);
      break;
    case OperState::kStoppingProcesses:
      s->set_oper_state(stagezero::proto::OPER_STOPPING_PROCESSES);
      break;
    }
    for (auto &proc : status.processes) {
      auto *p = s->add_processes();
      p->set_name(proc.name);
      p->set_process_id(proc.process_id);
      p->set_pid(proc.pid);
      p->set_running(proc.running);
    }
    break;
  }
  case EventType::kAlarm: {
    Alarm alarm = std::get<1>(event);
    alarm.ToProto(dest->mutable_alarm());
    break;
  }
  }
}

absl::Status Event::FromProto(const proto::Event &src) {
  switch (src.event_case()) {
  case stagezero::proto::Event::kSubsystemStatus: {
    const auto &s = src.subsystem_status();
    this->type = EventType::kSubsystemStatus;
    SubsystemStatusEvent status;
    status.subsystem = s.name();
    switch (s.admin_state()) {
    case stagezero::proto::ADMIN_OFFLINE:
      status.admin_state = AdminState::kOffline;
      break;
    case stagezero::proto::ADMIN_ONLINE:
      status.admin_state = AdminState::kOnline;
      break;
    default:
      return absl::InternalError(
          absl::StrFormat("Unknown admin state %d", s.admin_state()));
    }
    switch (s.oper_state()) {
    case stagezero::proto::OPER_OFFLINE:
      status.oper_state = OperState::kOffline;
      break;
    case stagezero::proto::OPER_STARTING_CHILDREN:
      status.oper_state = OperState::kStartingChildren;
      break;
    case stagezero::proto::OPER_STARTING_PROCESSES:
      status.oper_state = OperState::kStartingProcesses;
      break;
    case stagezero::proto::OPER_ONLINE:
      status.oper_state = OperState::kOnline;
      break;
    case stagezero::proto::OPER_STOPPING_CHILDREN:
      status.oper_state = OperState::kStoppingChildren;
      break;
    case stagezero::proto::OPER_STOPPING_PROCESSES:
      status.oper_state = OperState::kStoppingProcesses;
      break;
    case stagezero::proto::OPER_RESTARTING:
      status.oper_state = OperState::kRestarting;
      break;
    case stagezero::proto::OPER_BROKEN:
      status.oper_state = OperState::kBroken;
      break;
    default:
      return absl::InternalError(
          absl::StrFormat("Unknown oper state %d", s.oper_state()));
    }

    // Add the processes status.
    for (auto &proc : s.processes()) {
      status.processes.push_back({.name = proc.name(),
                                  .process_id = proc.process_id(),
                                  .pid = proc.pid(),
                                  .running = proc.running()});
    }
    this->event = status;
    break;
  }

  case stagezero::proto::Event::kAlarm: {
    Alarm alarm;
    alarm.FromProto(src.alarm());
    this->event = alarm;
    this->type = EventType::kAlarm;

    break;
  }
  default:
    // Unknown event type.
    return absl::InternalError(
        absl::StrFormat("Unknown event type %d", src.event_case()));
  }
  return absl::OkStatus();
}

} // namespace stagezero
