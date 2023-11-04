#pragma once

#include "proto/capcom.pb.h"
#include <string>

namespace stagezero::capcom {

struct Alarm {
  enum class Type {
    kUnknown,
    kProcess,
    kSubsystem,
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
  };

  enum class Status {
    kUnknown,
    kRaised,
    kCleared,
  };

  std::string name;
  Type type;
  Severity severity;
  Reason reason;
  Status status;
  std::string details;

  void ToProto(proto::Alarm *dest) const;
  void FromProto(const proto::Alarm &src);
};

} // namespace stagezero::capcom