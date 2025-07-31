#pragma once

#include "common/alarm.h"
#include "common/log.h"
#include "common/states.h"
#include <string>
#include <variant>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/parameters.h"
#include "common/subsystem_status.h"
#include "proto/event.pb.h"
#include "proto/parameters.pb.h"
#include "proto/subsystem_status.pb.h"
#include "proto/telemetry.pb.h"

namespace adastra {

// Event masks.  These control what type of events the client
// wants to see.
constexpr int kNoEvents = 0;
constexpr int kAllEvents = -1;
constexpr int kSubsystemStatusEvents = 1;
constexpr int kAlarmEvents = 2;
constexpr int kLogMessageEvents = 4;
constexpr int kOutputEvents = 8;
constexpr int kParameterEvents = 16;
constexpr int kTelemetryEvents = 32;

enum class EventType {
  kSubsystemStatus,
  kAlarm,
  kOutput,
  kLog,
  kParameterUpdate,
  kParameterDelete,
  kTelemetry,
};

// Alarm is defined in common/alarm.h

struct Output {
  std::string process_id;
  std::string data;
  int fd;
  std::string name;
};

struct Event {
  EventType type;
  std::variant<SubsystemStatus, Alarm, Output, LogMessage,
               parameters::Parameter, std::string,
               ::adastra::proto::TelemetryEvent>
      event;

  void ToProto(proto::Event *dest) const;
  absl::Status FromProto(const proto::Event &src);

  bool IsMaskedIn(int mask) const {
    switch (type) {
    case EventType::kAlarm:
      return (mask & kAlarmEvents) != 0;
    case EventType::kLog:
      return (mask & kLogMessageEvents) != 0;
    case EventType::kOutput:
      return (mask & kOutputEvents) != 0;
    case EventType::kSubsystemStatus:
      return (mask & kSubsystemStatusEvents) != 0;
    case EventType::kParameterUpdate:
    case EventType::kParameterDelete:
      return (mask & kParameterEvents) != 0;
    case EventType::kTelemetry:
      return (mask & kTelemetryEvents) != 0;
    }
  }
};

} // namespace adastra
