// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <variant>
#include "absl/status/status.h"
#include "absl/status/statusor.h"
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
    kInput,
    kOutput,
  };

  int stream_fd;
  bool tty = false;
  Disposition disposition = Disposition::kClient;
  Direction direction = Direction::kOutput;
  std::variant<std::string, int> data;

  void ToProto(proto::StreamControl *dest) const;
  absl::Status FromProto(const proto::StreamControl &src);
};

}  // namespace stagezero