// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "common/subsystem_status.h"
#include "absl/strings/str_format.h"
#include "proto/subsystem_status.pb.h"

namespace adastra {

void SubsystemStatus::ToProto(proto::SubsystemStatus *dest) const {
  dest->set_name(this->subsystem);
  switch (this->admin_state) {
  case AdminState::kOffline:
    dest->set_admin_state(adastra::proto::ADMIN_OFFLINE);
    break;
  case AdminState::kOnline:
    dest->set_admin_state(adastra::proto::ADMIN_ONLINE);
    break;
  }
  switch (this->oper_state) {
  case OperState::kBroken:
    dest->set_oper_state(adastra::proto::OPER_BROKEN);
    break;
  case OperState::kDegraded:
    dest->set_oper_state(adastra::proto::OPER_DEGRADED);
    break;
  case OperState::kOffline:
    dest->set_oper_state(adastra::proto::OPER_OFFLINE);
    break;
  case OperState::kOnline:
    dest->set_oper_state(adastra::proto::OPER_ONLINE);
    break;
  case OperState::kRestarting:
    dest->set_oper_state(adastra::proto::OPER_RESTARTING);
    break;
  case OperState::kRestartingProcesses:
    dest->set_oper_state(adastra::proto::OPER_RESTARTING_PROCESSES);
    break;
  case OperState::kStartingChildren:
    dest->set_oper_state(adastra::proto::OPER_STARTING_CHILDREN);
    break;
  case OperState::kStartingProcesses:
    dest->set_oper_state(adastra::proto::OPER_STARTING_PROCESSES);
    break;
  case OperState::kConnecting:
    dest->set_oper_state(adastra::proto::OPER_CONNECTING);
    break;
  case OperState::kStoppingChildren:
    dest->set_oper_state(adastra::proto::OPER_STOPPING_CHILDREN);
    break;
  case OperState::kStoppingProcesses:
    dest->set_oper_state(adastra::proto::OPER_STOPPING_PROCESSES);
    break;
  }
  dest->set_alarm_count(this->alarm_count);
  dest->set_restart_count(this->restart_count);
  for (auto &proc : this->processes) {
    auto *p = dest->add_processes();
    p->set_name(proc.name);
    p->set_process_id(proc.process_id);
    p->set_pid(proc.pid);
    p->set_running(proc.running);
    p->set_compute(proc.compute);
    p->set_subsystem(proc.subsystem);
    p->set_alarm_count(proc.alarm_count);
    switch (proc.type) {
    case ProcessType::kStatic:
      p->set_type(adastra::proto::SubsystemStatus::STATIC);
      break;
    case ProcessType::kZygote:
      p->set_type(adastra::proto::SubsystemStatus::ZYGOTE);
      break;
    case ProcessType::kVirtual:
      p->set_type(adastra::proto::SubsystemStatus::VIRTUAL);
      break;
    default:
      break;
    }
  }
}

absl::Status SubsystemStatus::FromProto(const proto::SubsystemStatus &src) {
  this->subsystem = src.name();
  switch (src.admin_state()) {
  case adastra::proto::ADMIN_OFFLINE:
    this->admin_state = AdminState::kOffline;
    break;
  case adastra::proto::ADMIN_ONLINE:
    this->admin_state = AdminState::kOnline;
    break;
  default:
    return absl::InternalError(
        absl::StrFormat("Unknown admin state %d", src.admin_state()));
  }
  switch (src.oper_state()) {
  case adastra::proto::OPER_OFFLINE:
    this->oper_state = OperState::kOffline;
    break;
  case adastra::proto::OPER_STARTING_CHILDREN:
    this->oper_state = OperState::kStartingChildren;
    break;
  case adastra::proto::OPER_CONNECTING:
    this->oper_state = OperState::kConnecting;
    break;
  case adastra::proto::OPER_STARTING_PROCESSES:
    this->oper_state = OperState::kStartingProcesses;
    break;
  case adastra::proto::OPER_ONLINE:
    this->oper_state = OperState::kOnline;
    break;
  case adastra::proto::OPER_STOPPING_CHILDREN:
    this->oper_state = OperState::kStoppingChildren;
    break;
  case adastra::proto::OPER_STOPPING_PROCESSES:
    this->oper_state = OperState::kStoppingProcesses;
    break;
  case adastra::proto::OPER_RESTARTING:
    this->oper_state = OperState::kRestarting;
    break;
  case adastra::proto::OPER_RESTARTING_PROCESSES:
    this->oper_state = OperState::kRestartingProcesses;
    break;
  case adastra::proto::OPER_BROKEN:
    this->oper_state = OperState::kBroken;
    break;
  case adastra::proto::OPER_DEGRADED:
    this->oper_state = OperState::kDegraded;
    break;
  default:
    return absl::InternalError(
        absl::StrFormat("Unknown oper state %d", src.oper_state()));
  }
  this->alarm_count = src.alarm_count();
  this->restart_count = src.restart_count();

  // Add the processes this->
  for (auto &proc : src.processes()) {
    ProcessType type;
    switch (proc.type()) {
    case adastra::proto::SubsystemStatus::STATIC:
    default:
      type = ProcessType::kStatic;
      break;
    case adastra::proto::SubsystemStatus::ZYGOTE:
      type = ProcessType::kZygote;
      break;
    case adastra::proto::SubsystemStatus::VIRTUAL:
      type = ProcessType::kVirtual;
      break;
    }
    this->processes.push_back({.name = proc.name(),
                               .process_id = proc.process_id(),
                               .pid = proc.pid(),
                               .running = proc.running(),
                               .type = type,
                               .compute = proc.compute(),
                               .subsystem = proc.subsystem(),
                               .alarm_count = proc.alarm_count()});
  }
  return absl::OkStatus();
}

} // namespace adastra
