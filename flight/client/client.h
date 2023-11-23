// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/alarm.h"
#include "common/states.h"
#include "common/vars.h"
#include "coroutine.h"
#include "proto/capcom.pb.h"
#include "proto/config.pb.h"
#include "proto/flight.pb.h"
#include "toolbelt/sockets.h"
#include <variant>

namespace stagezero::flight::client {

enum class ClientMode {
  kBlocking,
  kNonBlocking,
};

enum class EventType {
  kSubsystemStatus,
  kAlarm,
};

struct ProcessStatus {
  std::string name;
  std::string process_id;
  int pid;
  bool running;
};

struct SubsystemStatusEvent {
  std::string subsystem;
  capcom::AdminState admin_state;
  capcom::OperState oper_state;
  // std::vector<capcom::ProcessStatus> processes;
};

// Alarm is defined in common/alarm.h

struct Event {
  EventType type;
  std::variant<SubsystemStatusEvent, capcom::Alarm> event;
};

class Client {
public:
  Client(ClientMode mode = ClientMode::kBlocking, co::Coroutine *co = nullptr)
      : mode_(mode), co_(co) {}
  ~Client() = default;

  absl::Status Init(toolbelt::InetAddress addr, const std::string &name,
                    co::Coroutine *c = nullptr);

  absl::Status LoadGraph(const std::string &filename,
                         co::Coroutine *c = nullptr);

  absl::Status StartSubsystem(const std::string &name,
                              co::Coroutine *c = nullptr);
  absl::Status StopSubsystem(const std::string &name,
                             co::Coroutine *c = nullptr);

  toolbelt::FileDescriptor GetEventFd() const {
    return event_socket_.GetFileDescriptor();
  }
#if 0
  // Wait for an incoming event.
  absl::StatusOr<Event> WaitForEvent(co::Coroutine *c = nullptr) {
    return ReadEvent(c);
  }
  absl::StatusOr<Event> ReadEvent(co::Coroutine *c = nullptr);
#endif

  absl::Status Abort(const std::string &reason, co::Coroutine *c = nullptr);

private:
  static constexpr size_t kMaxMessageSize = 4096;

#if 0
absl::Status WaitForSubsystemState(const std::string& subsystem,
                                           capcom::AdminState admin_state,
                                           capcom::OperState oper_state);
#endif

  absl::Status
  SendRequestReceiveResponse(const stagezero::flight::proto::Request &req,
                             stagezero::flight::proto::Response &response,
                             co::Coroutine *c);

  std::string name_ = "";
  ClientMode mode_;
  co::Coroutine *co_;
  toolbelt::TCPSocket command_socket_;
  char command_buffer_[kMaxMessageSize];

  toolbelt::TCPSocket event_socket_;
  char event_buffer_[kMaxMessageSize];
};

} // namespace stagezero::flight::client
