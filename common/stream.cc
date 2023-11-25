#include "common/stream.h"

namespace stagezero {

absl::Status Stream::FromProto(const proto::StreamControl &src) {
  stream_fd = src.stream_fd();
  tty = src.tty();
  bool direction_set = false;

  switch (src.disposition()) {

  case stagezero::proto::StreamControl::CLIENT:
    disposition = Stream::Disposition::kClient;
    break;
  case stagezero::proto::StreamControl::FILENAME:
    disposition = Stream::Disposition::kFile;
    break;
  case stagezero::proto::StreamControl::FD:
    disposition = Stream::Disposition::kFd;
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
    case stagezero::proto::StreamControl::INPUT:
      direction = Stream::Direction::kInput;
      break;
    case stagezero::proto::StreamControl::OUTPUT:
      direction = Stream::Direction::kOutput;
      break;
    default:
      return absl::InternalError("Unknown stream direction");
    }
  }
  return absl::OkStatus();
}

void Stream::ToProto(stagezero::proto::StreamControl *dest) const {
  dest->set_stream_fd(stream_fd);
  dest->set_tty(tty);
  bool direction_set = false;
  switch (disposition) {
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
    case Stream::Direction::kInput:
      dest->set_direction(stagezero::proto::StreamControl::INPUT);
      break;
    case Stream::Direction::kOutput:
      dest->set_direction(stagezero::proto::StreamControl::OUTPUT);
      break;
    }
  }
}

} // namespace stagezero