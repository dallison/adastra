#pragma once

#include <cstdint>
#include <string>

#include "proto/log.pb.h"
#include "toolbelt/logging.h"

namespace adastra {

struct LogMessage {
  std::string source;
  toolbelt::LogLevel level;
  std::string text;
  uint64_t timestamp;

  void ToProto(adastra::proto::LogMessage *dest) {
    dest->set_source(source);
    dest->set_text(text);
    dest->set_timestamp(timestamp);
    switch (level) {
    case toolbelt::LogLevel::kDebug:
      dest->set_level(adastra::proto::LogMessage::LOG_DBG);
      break;
    case toolbelt::LogLevel::kVerboseDebug:
      dest->set_level(adastra::proto::LogMessage::LOG_VERBOSE);
      break;
    case toolbelt::LogLevel::kInfo:
      dest->set_level(adastra::proto::LogMessage::LOG_INFO);
      break;
    case toolbelt::LogLevel::kWarning:
      dest->set_level(adastra::proto::LogMessage::LOG_WARNING);
      break;
    case toolbelt::LogLevel::kError:
    case toolbelt::LogLevel::kFatal:
      dest->set_level(adastra::proto::LogMessage::LOG_ERR);
      break;
    }
  }

  void FromProto(const adastra::proto::LogMessage &src) {
    source = src.source();
    text = src.text();
    timestamp = src.timestamp();
    switch (src.level()) {
    case adastra::proto::LogMessage::LOG_DBG:
      level = toolbelt::LogLevel::kDebug;
      break;
    case adastra::proto::LogMessage::LOG_VERBOSE:
      level = toolbelt::LogLevel::kVerboseDebug;
      break;
    case adastra::proto::LogMessage::LOG_INFO:
      level = toolbelt::LogLevel::kInfo;
      break;
    case adastra::proto::LogMessage::LOG_WARNING:
      level = toolbelt::LogLevel::kWarning;
      break;
    case adastra::proto::LogMessage::LOG_ERR:
      level = toolbelt::LogLevel::kError;
      break;
    default:
    case adastra::proto::LogMessage::LOG_UNKNOWN:
      level = toolbelt::LogLevel::kVerboseDebug;
      break;
    }
  }
};

} // namespace adastra
