#pragma once

#include <cstdint>
#include <string>

#include "proto/log.pb.h"
#include "toolbelt/logging.h"

namespace stagezero {

struct LogMessage {
  enum class Level {
    kUnknown,
    kVerbose,
    kDebug,
    kInfo,
    kWarning,
    kError,
  };
  std::string source;
  Level level;
  std::string text;
  uint64_t timestamp;

  void ToProto(stagezero::proto::LogMessage *dest) {
    dest->set_source(source);
    dest->set_text(text);
    dest->set_timestamp(timestamp);
    switch (level) {
    case Level::kDebug:
      dest->set_level(stagezero::proto::LogMessage::DBG);
      break;
    case Level::kVerbose:
      dest->set_level(stagezero::proto::LogMessage::VERBOSE);
      break;
    case Level::kInfo:
      dest->set_level(stagezero::proto::LogMessage::INFO);
      break;
    case Level::kWarning:
      dest->set_level(stagezero::proto::LogMessage::WARNING);
      break;
    case Level::kError:
      dest->set_level(stagezero::proto::LogMessage::ERR);
      break;
    case Level::kUnknown:
      dest->set_level(stagezero::proto::LogMessage::UNKNOWN);
      break;
    }
  }

  void FromProto(const stagezero::proto::LogMessage &src) {
    source = src.source();
    text = src.text();
    timestamp = src.timestamp();
    switch (src.level()) {
    case stagezero::proto::LogMessage::DBG:
      level = Level::kDebug;
      break;
    case stagezero::proto::LogMessage::VERBOSE:
      level = Level::kVerbose;
      break;
    case stagezero::proto::LogMessage::INFO:
      level = Level::kInfo;
      break;
    case stagezero::proto::LogMessage::WARNING:
      level = Level::kWarning;
      break;
    case stagezero::proto::LogMessage::ERR:
      level = Level::kError;
      break;
    default:
    case stagezero::proto::LogMessage::UNKNOWN:
      level = Level::kUnknown;
      break;
    }
  }
};

inline LogMessage::Level ToLevel(toolbelt::LogLevel level) {
  switch (level) {
  case toolbelt::LogLevel::kVerboseDebug:
    return LogMessage::Level::kVerbose;
  case toolbelt::LogLevel::kDebug:
    return LogMessage::Level::kDebug;
  case toolbelt::LogLevel::kInfo:
    return LogMessage::Level::kInfo;
  case toolbelt::LogLevel::kWarning:
    return LogMessage::Level::kWarning;
  case toolbelt::LogLevel::kError:
    return LogMessage::Level::kError;
  case toolbelt::LogLevel::kFatal:
    // Fatal not supported here.
    return LogMessage::Level::kVerbose;
  }
}


} // namespace stagezero