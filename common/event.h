#pragma once

#include <string>
#include <variant>
#include "common/alarm.h"
#include "common/states.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/subsystem_status.h"
#include "proto/event.pb.h"
#include "proto/subsystem_status.pb.h"

namespace stagezero {

enum class EventType {
  kSubsystemStatus,
  kAlarm,
  kOutput,
};

// Alarm is defined in common/alarm.h

struct Output {
  std::string process_id;
  std::string data;
  int fd;
};

struct Event {
  EventType type;
  std::variant<SubsystemStatus, Alarm, Output> event;

  void ToProto(proto::Event *dest) const;
  absl::Status FromProto(const proto::Event &src);
};

}  // namespace stagezero