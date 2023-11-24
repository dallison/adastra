#pragma once

#include "common/alarm.h"
#include "common/states.h"
#include <string>
#include <variant>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace stagezero {

enum class EventType {
  kSubsystemStatus,
  kAlarm,
};

struct ProcessStatus {
  std::string name;
  std::string process_id;
  int pid;
  bool running;
};

struct SubsystemStatusEvent {
  std::string subsystem;
  AdminState admin_state;
  OperState oper_state;
  std::vector<ProcessStatus> processes;
};

// Alarm is defined in common/alarm.h

struct Event {
  EventType type;
  std::variant<SubsystemStatusEvent, Alarm> event;

  void ToProto(proto::Event *dest) const;
  absl::Status FromProto(const proto::Event &src);
};

} // namespace stagezero