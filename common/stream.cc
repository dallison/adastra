// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "common/stream.h"

namespace stagezero {

void Terminal::ToProto(proto::Terminal *dest) const {
  dest->set_name(name);
  dest->set_rows(rows);
  dest->set_cols(cols);
}

void Terminal::FromProto(const proto::Terminal &src) {
  rows = src.rows();
  cols = src.cols();
  name = src.name();
}

absl::Status ValidateStreams(
    google::protobuf::RepeatedPtrField<proto::StreamControl> streams) {
  for (auto &stream : streams) {

    if (stream.disposition() == proto::StreamControl::CLOSE ||
        stream.disposition() == proto::StreamControl::STAGEZERO) {
      return absl::OkStatus();
    }

    switch (stream.stream_fd()) {
    case STDIN_FILENO:
      if (stream.direction() == proto::StreamControl::OUTPUT) {
        return absl::InternalError("Invalid stream direction for stdin");
      }
      break;
    case STDOUT_FILENO:
    case STDERR_FILENO:
      if (stream.direction() == proto::StreamControl::INPUT) {
        return absl::InternalError(absl::StrFormat(
            "Invalid stream direction for std%s",
            stream.stream_fd() == STDOUT_FILENO ? "out" : "err"));
      }
      break;
    default:
      if (stream.direction() == proto::StreamControl::DEFAULT) {
        return absl::InternalError(absl::StrFormat(
            "You need to specify a direction for fd %d", stream.stream_fd()));
      }
      break;
    }
  }
  return absl::OkStatus();
}

void AddStream(std::vector<Stream> &streams, const Stream &stream) {
  for (auto &s : streams) {
    if (s.stream_fd == stream.stream_fd) {
      s = stream;
      return;
    }
  }
  streams.push_back(stream);
}

absl::Status Stream::FromProto(const proto::StreamControl &src) {
  stream_fd = src.stream_fd();
  tty = src.tty();
  bool direction_set = false;

  switch (src.disposition()) {
  case stagezero::proto::StreamControl::STAGEZERO:
    disposition = Stream::Disposition::kStageZero;
    break;
  case stagezero::proto::StreamControl::CLIENT:
    disposition = Stream::Disposition::kClient;
    break;
  case stagezero::proto::StreamControl::FILENAME:
    disposition = Stream::Disposition::kFile;
    data = src.filename();
    break;
  case stagezero::proto::StreamControl::FD:
    disposition = Stream::Disposition::kFd;
    data = src.fd();
    break;
  case stagezero::proto::StreamControl::CLOSE:
    disposition = Stream::Disposition::kClose;
    break;
  case stagezero::proto::StreamControl::LOGGER:
    disposition = Stream::Disposition::kLog;
    direction = Stream::Direction::kOutput;
    direction_set = true;
    break;
  default:
    return absl::InternalError("Unknown stream disposition");
  }

  if (!direction_set) {
    switch (src.direction()) {
    case stagezero::proto::StreamControl::DEFAULT:
      direction = Stream::Direction::kDefault;
      break;
    case stagezero::proto::StreamControl::INPUT:
      direction = Stream::Direction::kInput;
      break;
    case stagezero::proto::StreamControl::OUTPUT:
      direction = Stream::Direction::kOutput;
      break;
    default:
      return absl::InternalError("Unknown stream direction");
    }

    if (src.has_terminal()) {
      auto &t = src.terminal();
      terminal.FromProto(t);
    }
  }

  return absl::OkStatus();
}

void Stream::ToProto(stagezero::proto::StreamControl *dest) const {
  dest->set_stream_fd(stream_fd);
  dest->set_tty(tty);
  bool direction_set = false;
  switch (disposition) {
  case Stream::Disposition::kStageZero:
    dest->set_disposition(stagezero::proto::StreamControl::STAGEZERO);
    break;
  case Stream::Disposition::kClient:
    dest->set_disposition(stagezero::proto::StreamControl::CLIENT);
    break;
  case Stream::Disposition::kFile:
    dest->set_disposition(stagezero::proto::StreamControl::FILENAME);
    dest->set_filename(std::get<0>(data));
    break;
  case Stream::Disposition::kFd:
    dest->set_disposition(stagezero::proto::StreamControl::FD);
    dest->set_fd(std::get<1>(data));
    break;
  case Stream::Disposition::kClose:
    dest->set_disposition(stagezero::proto::StreamControl::CLOSE);
    break;
  case Stream::Disposition::kLog:
    dest->set_disposition(stagezero::proto::StreamControl::LOGGER);
    dest->set_direction(stagezero::proto::StreamControl::OUTPUT);
    direction_set = true;
    break;
  }

  if (!direction_set) {
    switch (direction) {
    case Stream::Direction::kDefault:
      dest->set_direction(stagezero::proto::StreamControl::DEFAULT);
      break;
    case Stream::Direction::kInput:
      dest->set_direction(stagezero::proto::StreamControl::INPUT);
      break;
    case Stream::Direction::kOutput:
      dest->set_direction(stagezero::proto::StreamControl::OUTPUT);
      break;
    }
  }

  if (terminal.cols != 0 || terminal.rows != 0 || !terminal.name.empty()) {
    auto t = dest->mutable_terminal();
    terminal.ToProto(t);
  }
}

} // namespace stagezero