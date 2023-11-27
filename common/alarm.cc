// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "common/alarm.h"
#include "proto/event.pb.h"

namespace stagezero {

void Alarm::ToProto(proto::Alarm *dest) const {
  dest->set_id(id);
  dest->set_name(name);
  dest->set_details(details);
  switch (type) {
    case Type::kProcess:
      dest->set_type(proto::Alarm::PROCESS);
      break;
    case Type::kSubsystem:
      dest->set_type(proto::Alarm::SUBSYSTEM);
      break;
    case Type::kUnknown:
      dest->set_type(proto::Alarm::UNKNOWN_TYPE);
      break;
  }

  switch (severity) {
    case Severity::kWarning:
      dest->set_severity(proto::Alarm::WARNING);
      break;
    case Severity::kError:
      dest->set_severity(proto::Alarm::ERROR);
      break;
    case Severity::kCritical:
      dest->set_severity(proto::Alarm::CRITICAL);
      break;
    case Severity::kUnknown:
      dest->set_severity(proto::Alarm::UNKNOWN_SEVERITY);
      break;
  }

  switch (reason) {
    case Reason::kCrashed:
      dest->set_reason(proto::Alarm::CRASHED);
      break;
    case Reason::kBroken:
      dest->set_reason(proto::Alarm::BROKEN);
      break;
    case Reason::kUnknown:
      dest->set_reason(proto::Alarm::UNKNOWN_REASON);
      break;
  }

  switch (status) {
    case Status::kRaised:
      dest->set_status(proto::Alarm::RAISED);
      break;
    case Status::kCleared:
      dest->set_status(proto::Alarm::CLEARED);
      break;
    case Status::kUnknown:
      dest->set_status(proto::Alarm::UNKNOWN_STATUS);
      break;
  }
}

void Alarm::FromProto(const proto::Alarm &src) {
  id = src.id();
  name = src.name();
  details = src.details();
  switch (src.type()) {
    case proto::Alarm::PROCESS:
      type = Type::kProcess;
      break;
    case proto::Alarm::SUBSYSTEM:
      type = Type::kSubsystem;
      break;
    default:
      type = Type::kUnknown;
      break;
  }

  switch (src.severity()) {
    case proto::Alarm::WARNING:
      severity = Severity::kWarning;
      break;
    case proto::Alarm::ERROR:
      severity = Severity::kError;
      break;
    case proto::Alarm::CRITICAL:
      severity = Severity::kCritical;
      break;
    default:
      severity = Severity::kUnknown;
      break;
  }

  switch (src.reason()) {
    case proto::Alarm::CRASHED:
      reason = Reason::kCrashed;
      break;
    case proto::Alarm::BROKEN:
      reason = Reason::kBroken;
      break;
    default:
      reason = Reason::kUnknown;
      break;
  }

  switch (src.status()) {
    case proto::Alarm::RAISED:
      status = Status::kRaised;
      break;
    case proto::Alarm::CLEARED:
      status = Status::kCleared;
      break;
    default:
      status = Status::kUnknown;
      break;
  }
}
}  // namespace stagezero