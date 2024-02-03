// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "proto/event.pb.h"
#include <string>

namespace adastra {

struct Alarm {
  enum class Type {
    kUnknown,
    kProcess,
    kSubsystem,
    kSystem,
  };

  enum class Severity {
    kUnknown,
    kWarning,
    kError,
    kCritical,
  };

  enum class Reason {
    kUnknown,
    kCrashed,
    kBroken,
    kEmergencyAbort,
  };

  enum class Status {
    kUnknown,
    kRaised,
    kCleared,
  };

  std::string id;
  std::string name;
  Type type;
  Severity severity;
  Reason reason;
  Status status;
  std::string details;

  void ToProto(proto::Alarm *dest) const;
  void FromProto(const proto::Alarm &src);
};

inline const char *TypeName(Alarm::Type type) {
  switch (type) {
  case Alarm::Type::kProcess:
    return "process";
  case Alarm::Type::kSubsystem:
    return "subsystem";
  default:
    return "unknown";
  }
}

inline const char *SeverityName(Alarm::Severity s) {
  switch (s) {
  case Alarm::Severity::kWarning:
    return "warning";
  case Alarm::Severity::kError:
    return "error";
  case Alarm::Severity::kCritical:
    return "critical";
  default:
    return "unknown";
  }
}

inline const char *ReasonName(Alarm::Reason r) {
  switch (r) {
  case Alarm::Reason::kCrashed:
    return "crashed";
  case Alarm::Reason::kBroken:
    return "broken";
  default:
    return "unknown";
  }
}

inline const char *StatusName(Alarm::Status s) {
  switch (s) {
  case Alarm::Status::kRaised:
    return "raised";
  case Alarm::Status::kCleared:
    return "cleared";
  default:
    return "unknown";
  }
}

inline std::ostream &operator<<(std::ostream &os, const Alarm &alarm) {
  os << "id: " << alarm.id << " name: " << alarm.name
     << " status: " << StatusName(alarm.status)
     << " type: " << TypeName(alarm.type)
     << " severity: " << SeverityName(alarm.severity)
     << " reason: " << ReasonName(alarm.reason)
     << " details: " << alarm.details;
  return os;
}

} // namespace adastra
