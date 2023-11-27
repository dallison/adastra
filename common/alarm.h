// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <string>
#include "proto/event.pb.h"

namespace stagezero {

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

}  // namespace stagezero