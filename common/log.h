#pragma once

#include <cstdint>
#include <string>

#include "proto/log.pb.h"
#include "toolbelt/logging.h"

namespace stagezero {

struct LogMessage {
  std::string source;
  toolbelt::LogLevel level;
  std::string text;
  uint64_t timestamp;

  void ToProto(stagezero::proto::LogMessage *dest) {
    dest->set_source(source);
    dest->set_text(text);
    dest->set_timestamp(timestamp);
    switch (level) {
    case toolbelt::LogLevel::kDebug:
      dest->set_level(stagezero::proto::LogMessage::DBG);
      break;
    case toolbelt::LogLevel::kVerboseDebug:
      dest->set_level(stagezero::proto::LogMessage::VERBOSE);
      break;
    case toolbelt::LogLevel::kInfo:
      dest->set_level(stagezero::proto::LogMessage::INFO);
      break;
    case toolbelt::LogLevel::kWarning:
      dest->set_level(stagezero::proto::LogMessage::WARNING);
      break;
    case toolbelt::LogLevel::kError:
    case toolbelt::LogLevel::kFatal:
      dest->set_level(stagezero::proto::LogMessage::ERR);
      break;
    }
  }

  void FromProto(const stagezero::proto::LogMessage &src) {
    source = src.source();
    text = src.text();
    timestamp = src.timestamp();
    switch (src.level()) {
    case stagezero::proto::LogMessage::DBG:
      level = toolbelt::LogLevel::kDebug;
      break;
    case stagezero::proto::LogMessage::VERBOSE:
      level = toolbelt::LogLevel::kVerboseDebug;
      break;
    case stagezero::proto::LogMessage::INFO:
      level = toolbelt::LogLevel::kInfo;
      break;
    case stagezero::proto::LogMessage::WARNING:
      level = toolbelt::LogLevel::kWarning;
      break;
    case stagezero::proto::LogMessage::ERR:
      level = toolbelt::LogLevel::kError;
      break;
    default:
    case stagezero::proto::LogMessage::UNKNOWN:
      level = toolbelt::LogLevel::kVerboseDebug;
      break;
    }
  }
};

} // namespace stagezero