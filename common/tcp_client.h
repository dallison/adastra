// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "coroutine.h"
#include "toolbelt/sockets.h"
#include <memory>

namespace adastra {

template <typename Request, typename Response, typename Event> class TCPClient {
public:
  TCPClient(co::Coroutine *co = nullptr) : co_(co) {}
  ~TCPClient() = default;

  absl::Status
  Init(toolbelt::InetAddress addr, const std::string &name,
       std::function<void(Request &)> fill_request,
       std::function<absl::StatusOr<int>(const Response &)> parse_response,
       co::Coroutine *c = nullptr);

  toolbelt::FileDescriptor GetEventFd() const {
    return event_socket_.GetFileDescriptor();
  }

  void Close() {
    command_socket_.Close();
    event_socket_.Close();
  }

  void Reset() {
    command_socket_ = {};
    event_socket_ = {};
  }

  bool IsConnected() const {
    return command_socket_.Connected() && event_socket_.Connected();
  }

  // Wait for an incoming event.
  absl::StatusOr<std::shared_ptr<Event>>
  WaitForProtoEvent(co::Coroutine *c = nullptr) {
    return ReadProtoEvent(c);
  }
  absl::StatusOr<std::shared_ptr<Event>>
  ReadProtoEvent(co::Coroutine *c = nullptr);

protected:
  static constexpr size_t kMaxMessageSize = 4096;

  absl::Status SendRequestReceiveResponse(const Request &req,
                                          Response &response, co::Coroutine *c);

  std::string name_ = "";
  co::Coroutine *co_;
  toolbelt::TCPSocket command_socket_;

  toolbelt::TCPSocket event_socket_;
  char event_buffer_[kMaxMessageSize];
};

template <typename Request, typename Response, typename Event>
absl::Status TCPClient<Request, Response, Event>::Init(
    toolbelt::InetAddress addr, const std::string &name,
    std::function<void(Request &)> fill_request,
    std::function<absl::StatusOr<int>(const Response &)> parse_response,
    co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  absl::Status status = command_socket_.Connect(addr);
  if (!status.ok()) {
    return status;
  }

  if (absl::Status status = command_socket_.SetCloseOnExec(); !status.ok()) {
    return status;
  }

  name_ = name;

  Request req;
  fill_request(req);

  Response resp;
  status = SendRequestReceiveResponse(req, resp, c);
  if (!status.ok()) {
    return absl::InternalError(
        absl::StrFormat("Failed to send command: %s", status.ToString()));
  }

  absl::StatusOr<int> event_port = parse_response(resp);
  if (!event_port.ok()) {
    return event_port.status();
  }
  toolbelt::InetAddress event_addr = addr;
  event_addr.SetPort(*event_port);

  if (absl::Status status = event_socket_.Connect(event_addr); !status.ok()) {
    return status;
  }

  if (absl::Status status = event_socket_.SetCloseOnExec(); !status.ok()) {
    return status;
  }

  return absl::OkStatus();
}

template <typename Request, typename Response, typename Event>
absl::StatusOr<std::shared_ptr<Event>>
TCPClient<Request, Response, Event>::ReadProtoEvent(co::Coroutine *c) {
  if (c == nullptr) {
    c = co_;
  }
  auto event = std::make_shared<Event>();

  absl::StatusOr<ssize_t> n =
      event_socket_.ReceiveMessage(event_buffer_, sizeof(event_buffer_), c);
  if (!n.ok() || *n == 0) {
    event_socket_.Close();
    // On error or EOF, tell the caller that the socket has been closed.
    return absl::CancelledError(
        absl::StrFormat("Event channel error: %s", n.status().ToString()));
  }

  if (!event->ParseFromArray(event_buffer_, *n)) {
    event_socket_.Close();
    return absl::InternalError("Failed to parse event");
  }

  return event;
}

template <typename Request, typename Response, typename Event>
absl::Status TCPClient<Request, Response, Event>::SendRequestReceiveResponse(
    const Request &req, Response &response, co::Coroutine *c) {
  // SendMessage needs 4 bytes before the buffer passed to
  // use for the length.
  uint64_t reqlen = req.ByteSizeLong();
  std::vector<char> command_buffer(reqlen + sizeof(int32_t));
  char *sendbuf = command_buffer.data() + sizeof(int32_t);

  if (!req.SerializeToArray(sendbuf, reqlen)) {
    return absl::InternalError("Failed to serialize request");
  }

  absl::StatusOr<ssize_t> n = command_socket_.SendMessage(sendbuf, reqlen, c);
  if (!n.ok()) {
    command_socket_.Close();
    return n.status();
  }

  absl::StatusOr<std::vector<char>> respbuffer =
      command_socket_.ReceiveVariableLengthMessage(c);

  if (!respbuffer.ok()) {
    command_socket_.Close();
    return n.status();
  }

  if (!response.ParseFromArray(respbuffer->data(), respbuffer->size())) {
    command_socket_.Close();
    return absl::InternalError("Failed to parse response");
  }

  return absl::OkStatus();
}
} // namespace adastra
