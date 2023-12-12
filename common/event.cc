// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "common/event.h"
#include "absl/strings/str_format.h"
#include "common/subsystem_status.h"
#include "proto/event.pb.h"
#include "proto/subsystem_status.pb.h"

namespace stagezero {

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
  }
}

absl::Status Event::FromProto(const proto::Event &src) {
  switch (src.event_case()) {
  case stagezero::proto::Event::kSubsystemStatus: {
    const auto &s = src.subsystem_status();
    this->type = EventType::kSubsystemStatus;
    SubsystemStatus status;
    if (absl::Status stat = status.FromProto(s); !stat.ok()) {
      return stat;
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

  case stagezero::proto::Event::kOutput: {
    Output output = {
        .process_id = src.output().process_id(), .data = src.output().data(), .fd = src.output().fd()};
    this->event = output;
    this->type = EventType::kOutput;
    break;
  }

  case stagezero::proto::Event::kLog: {
    LogMessage log;
    log.FromProto(src.log());
    this->event = log;
    this->type = EventType::kLog;
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
