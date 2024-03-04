// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "proto/stream.pb.h"
#include <variant>

namespace adastra {

struct Terminal {
  std::string name;
  int rows = 0;
  int cols = 0;

  bool IsPresent() const { return rows != 0 || cols != 0 || !name.empty();}
  void ToProto(stagezero::proto::Terminal *dest) const;
  void FromProto(const stagezero::proto::Terminal &src);
};

struct Stream {
  enum class Disposition {
    kStageZero,
    kClient,
    kFile,
    kFd,
    kClose,
    kLog,
    kSyslog,
  };

  enum class Direction {
    kDefault,
    kInput,
    kOutput,
  };

  int stream_fd;
  bool tty = false;
  Disposition disposition = Disposition::kStageZero;
  Direction direction = Direction::kDefault;
  std::variant<std::string, int> data;
  Terminal terminal;

  void ToProto(stagezero::proto::StreamControl *dest) const;
  absl::Status FromProto(const stagezero::proto::StreamControl &src);
};

absl::Status ValidateStreams(
    google::protobuf::RepeatedPtrField<stagezero::proto::StreamControl> streams);

void AddStream(std::vector<Stream> &streams, const Stream &stream);

} // namespace adastra
