#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "stagezero/client/client.h"
#include "capcom/subsystem.h"
#include "capcom/client_handler.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"

#include <memory>
#include <string>

namespace stagezero::capcom {

constexpr int64_t kReady = 1;
constexpr int64_t kStopped = 2;

class ClientHandler;

class Capcom {
public:
  Capcom(co::CoroutineScheduler &scheduler,
                     toolbelt::InetAddress addr, int notify_fd,
                     toolbelt::InetAddress stagezero);
  ~Capcom();

  absl::Status Run();
  void Stop();

private:
  friend class ClientHandler;
  friend class Subsystem;
  const toolbelt::InetAddress GetStageZeroAddress() const { return stagezero_; }

  absl::Status HandleIncomingConnection(toolbelt::TCPSocket &listen_socket,
                                        co::Coroutine *c);

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) {
    coroutines_.insert(std::move(c));
  }

  void CloseHandler(std::shared_ptr<ClientHandler> handler);
  void ListenerCoroutine(toolbelt::TCPSocket &listen_socket, co::Coroutine *c);

  bool AddSubsystem(std::string name, std::shared_ptr<Subsystem> subsystem) {
    std::cerr << "adding subsystem " << name << std::endl;
    auto[it, inserted] =
        subsystems_.emplace(std::make_pair(std::move(name), std::move(subsystem)));
    return inserted;
  }

 absl::Status RemoveSubsystem(const std::string& name) {
    std::cerr << "Removing subsystem " << name << std::endl;
  
    auto it = subsystems_.find(name);
    if (it == subsystems_.end()) {
      return absl::InternalError(absl::StrFormat("No such subsystem %s", name));
    }
    subsystems_.erase(it);
    return absl::OkStatus();
  }

  std::shared_ptr<Subsystem> FindSubsystem(const std::string& name) const {
        auto it = subsystems_.find(name);
    if (it == subsystems_.end()) {
      return nullptr;
    }
    return it->second;
  }

  void SendSubsystemStatusEvent(std::shared_ptr<Subsystem> subsystem);

private:
  co::CoroutineScheduler &co_scheduler_;
  toolbelt::InetAddress addr_;
  toolbelt::FileDescriptor notify_fd_;
  toolbelt::InetAddress stagezero_;

  // All coroutines are owned by this set.
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> coroutines_;

  std::vector<std::shared_ptr<ClientHandler>> client_handlers_;
  bool running_ = false;
  absl::flat_hash_map<std::string, std::shared_ptr<Subsystem>> subsystems_;
  toolbelt::Logger logger_;

  std::unique_ptr<stagezero::Client> main_client_;
};
}