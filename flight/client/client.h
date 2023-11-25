// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "common/alarm.h"
#include "common/event.h"
#include "common/subsystem_status.h"
#include "common/states.h"
#include "common/tcp_client.h"
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

class Client : public TCPClient<flight::proto::Request, flight::proto::Response,
                                stagezero::proto::Event> {
public:
  Client(ClientMode mode = ClientMode::kBlocking, co::Coroutine *co = nullptr)
      : TCPClient<flight::proto::Request, flight::proto::Response,
                  stagezero::proto::Event>(co),
        mode_(mode) {}
  ~Client() = default;

  absl::Status Init(toolbelt::InetAddress addr, const std::string &name,
                    co::Coroutine *c = nullptr);

  // Wait for an incoming event.
  absl::StatusOr<std::shared_ptr<Event>>
  WaitForEvent(co::Coroutine *c = nullptr) {
    return ReadEvent(c);
  }
  absl::StatusOr<std::shared_ptr<Event>> ReadEvent(co::Coroutine *c = nullptr);

  absl::Status LoadGraph(const std::string &filename,
                         co::Coroutine *c = nullptr);

  absl::Status StartSubsystem(const std::string &name,
                              co::Coroutine *c = nullptr);
  absl::Status StopSubsystem(const std::string &name,
                             co::Coroutine *c = nullptr);

  absl::Status Abort(const std::string &reason, co::Coroutine *c = nullptr);
  absl::StatusOr<std::vector<SubsystemStatus>> GetSubsystems(co::Coroutine *c = nullptr);

private:
  absl::Status WaitForSubsystemState(const std::string &subsystem,
                                     AdminState admin_state,
                                     OperState oper_state, co::Coroutine* c = nullptr);

  ClientMode mode_;
};

} // namespace stagezero::flight::client
