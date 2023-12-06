// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <variant>
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "proto/stream.pb.h"

namespace stagezero {

struct Stream {
  enum class Disposition {
    kStageZero,
    kClient,
    kFile,
    kFd,
    kClose,
    kLog,
  };

  enum class Direction {
    kDefault,
    kInput,
    kOutput,
  };

  struct WindowSize {
    int rows = 0;
    int cols = 0;
  };

  int stream_fd;
  bool tty = false;
  Disposition disposition = Disposition::kStageZero;
  Direction direction = Direction::kDefault;
  std::variant<std::string, int> data;
  WindowSize window_size;
  std::string term_name;

  void ToProto(proto::StreamControl *dest) const;
  absl::Status FromProto(const proto::StreamControl &src);
};

absl::Status ValidateStreams(
    google::protobuf::RepeatedPtrField<proto::StreamControl> streams);

void AddStream(std::vector<Stream>& streams, const Stream& stream);

}  // namespace stagezero