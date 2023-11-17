#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "flight/client_handler.h"
#include "capcom/client/client.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "capcom/client/client.h"

#include <memory>
#include <string>

namespace stagezero::flight {

constexpr int64_t kReady = 1;
constexpr int64_t kStopped = 2;

class ClientHandler;

class FlightDirector {
public:
  FlightDirector(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
  toolbelt::InetAddress capcom_addr,
         int notify_fd);
  ~FlightDirector();

  absl::Status Run();
  void Stop();

private:
  friend class ClientHandler;

  absl::Status HandleIncomingConnection(toolbelt::TCPSocket &listen_socket,
                                        co::Coroutine *c);

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) {
    coroutines_.insert(std::move(c));
  }

  void CloseHandler(std::shared_ptr<ClientHandler> handler);
  void ListenerCoroutine(toolbelt::TCPSocket &listen_socket, co::Coroutine *c);

private:
  co::CoroutineScheduler &co_scheduler_;
  toolbelt::InetAddress addr_;
  toolbelt::InetAddress capcom_addr_;
  toolbelt::FileDescriptor notify_fd_;

  capcom::client::Client capcom_client_;

  // All coroutines are owned by this set.
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> coroutines_;

  std::vector<std::shared_ptr<ClientHandler>> client_handlers_;
  bool running_ = false;
  toolbelt::Logger logger_;
};
}

