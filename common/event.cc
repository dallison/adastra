// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "common/event.h"
#include "absl/strings/str_format.h"
#include "common/subsystem_status.h"
#include "proto/event.pb.h"
#include "proto/subsystem_status.pb.h"

namespace adastra {

void Event::ToProto(proto::Event *dest) const {
  switch (type) {
  case EventType::kSubsystemStatus: {
    SubsystemStatus status = std::get<0>(event);
    auto s = dest->mutable_subsystem_status();
    status.ToProto(s);
    break;
  }
  case EventType::kAlarm: {
    Alarm alarm = std::get<1>(event);
    alarm.ToProto(dest->mutable_alarm());
    break;
  }
  case EventType::kOutput: {
    Output output = std::get<2>(event);
    auto o = dest->mutable_output();
    o->set_process_id(output.process_id);
    o->set_data(output.data);
    o->set_fd(output.fd);
    break;
  }
  case EventType::kLog: {
    LogMessage log = std::get<3>(event);
    log.ToProto(dest->mutable_log());
    break;
  }
  case EventType::kParameterUpdate: {
    parameters::Parameter p = std::get<4>(event);
    auto pe = dest->mutable_parameter();
    p.ToProto(pe->mutable_update());
    break;
  }
  case EventType::kParameterDelete: {
    const std::string &name = std::get<5>(event);
    auto pe = dest->mutable_parameter();
    pe->set_delete_(name);
    break;
  }
  }
}

absl::Status Event::FromProto(const proto::Event &src) {
  switch (src.event_case()) {
  case adastra::proto::Event::kSubsystemStatus: {
    const auto &s = src.subsystem_status();
    this->type = EventType::kSubsystemStatus;
    SubsystemStatus status;
    if (absl::Status stat = status.FromProto(s); !stat.ok()) {
      return stat;
    }
    this->event = status;
    break;
  }

  case adastra::proto::Event::kAlarm: {
    Alarm alarm;
    alarm.FromProto(src.alarm());
    this->event = alarm;
    this->type = EventType::kAlarm;

    break;
  }

  case adastra::proto::Event::kOutput: {
    Output output = {.process_id = src.output().process_id(),
                     .data = src.output().data(),
                     .fd = src.output().fd()};
    this->event = output;
    this->type = EventType::kOutput;
    break;
  }

  case adastra::proto::Event::kLog: {
    LogMessage log;
    log.FromProto(src.log());
    this->event = log;
    this->type = EventType::kLog;
    break;
  }
  case adastra::proto::Event::kParameter: {
    switch (src.parameter().event_case()) {
    case adastra::proto::parameters::ParameterEvent::kUpdate: {
      parameters::Parameter p;
      p.FromProto(src.parameter().update());
      this->event = p;
      this->type = EventType::kParameterUpdate;
      break;
    }
    case adastra::proto::parameters::ParameterEvent::kDelete:
      this->event = src.parameter().delete_();
      this->type = EventType::kParameterDelete;
      break;
    default:
      return absl::InternalError(absl::StrFormat(
          "Unknown parameter event type %d", src.parameter().event_case()));
    }
    break;
    
  default:
    // Unknown event type.
    return absl::InternalError(
        absl::StrFormat("Unknown event type %d", src.event_case()));
  }
  }
  return absl::OkStatus();
}

} // namespace adastra
