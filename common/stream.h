#pragma once

#include "proto/stream.pb.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include <variant>

namespace stagezero {

struct Stream {
  enum class Disposition {
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


}