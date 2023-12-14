// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "capcom/client/client.h"
#include "common/vars.h"
#include "flight/client_handler.h"
#include "flight/subsystem.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"

#include <filesystem>
#include <memory>
#include <string>

namespace stagezero::flight {

constexpr int64_t kReady = 1;
constexpr int64_t kStopped = 2;

constexpr int kDefaultStartupTimeout = 10;
constexpr int kDefaultSigIntTimeout = 2;
constexpr int kDefaultSigTermTimeout = 15;

class ClientHandler;

struct Compute {
  std::string name;
  toolbelt::InetAddress addr;
};

class FlightDirector {
 public:
  FlightDirector(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
                 toolbelt::InetAddress capcom_addr, const std::string &root_dir,
                 bool log_to_output,
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
  void EventMonitorCoroutine(co::Coroutine *c);

  absl::Status LoadAllSubsystemGraphs(const std::filesystem::path &dir);
  absl::Status LoadAllSubsystemGraphsFromDir(
      const std::filesystem::path &dir,
      std::vector<std::unique_ptr<proto::SubsystemGraph>> &graphs);
  absl::StatusOr<std::unique_ptr<proto::SubsystemGraph>> PreloadSubsystemGraph(
      const std::filesystem::path &file);
  absl::Status LoadSubsystemGraph(std::unique_ptr<proto::SubsystemGraph> graph);
  absl::Status CheckForSubsystemLoops();
  absl::Status CheckForSubsystemLoopsRecurse(
      absl::flat_hash_set<Subsystem *> &visited, Subsystem *subsystem,
      std::string path);

  std::vector<Subsystem *> FlattenSubsystemGraph(Subsystem *root);
  void FlattenSubsystemGraphRecurse(absl::flat_hash_set<Subsystem *> &visited,
                                    Subsystem *subsystem,
                                    std::vector<Subsystem *> &vec);

  absl::Status RegisterSubsystemGraph(Subsystem *root, co::Coroutine *c);
  absl::Status RegisterCompute(const Compute &compute, co::Coroutine *c);
  absl::Status RegisterGlobalVariable(const Variable &compute,
                                      co::Coroutine *c);

  absl::Status AutostartSubsystem(Subsystem *subsystem, co::Coroutine *c);

  Subsystem *FindSubsystem(const std::string &name) const {
    auto it = subsystems_.find(name);
    if (it == subsystems_.end()) {
      return nullptr;
    }
    return it->second.get();
  }

  const Compute *FindCompute(const std::string &name) const {
    auto it = computes_.find(name);
    if (it == computes_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  void AddCompute(const std::string &name, const Compute &compute) {
    computes_.emplace(std::make_pair(name, std::move(compute)));
  }

  void AddGlobalVariable(Variable var) {
    global_variables_.push_back(std::move(var));
  }

  Process* FindInteractiveProcess(Subsystem* subsystem) const;

  void CacheEvent(std::shared_ptr<stagezero::Event> event);
  void DumpEventCache(ClientHandler* handler);

  co::CoroutineScheduler &co_scheduler_;
  toolbelt::InetAddress addr_;
  toolbelt::InetAddress capcom_addr_;
  std::string root_dir_;
  bool log_to_output_ = true;
  toolbelt::FileDescriptor notify_fd_;

  capcom::client::Client capcom_client_;
  std::unique_ptr<capcom::client::Client> autostart_capcom_client_;

  // All coroutines are owned by this set.
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> coroutines_;

  absl::flat_hash_map<std::string, std::unique_ptr<Subsystem>> subsystems_;
  absl::flat_hash_map<std::string, Subsystem *> autostarts_;
  absl::flat_hash_map<std::string, Subsystem *> interfaces_;

  absl::flat_hash_map<std::string, Compute> computes_;

  std::vector<Variable> global_variables_;

  std::vector<std::shared_ptr<ClientHandler>> client_handlers_;
  bool running_ = false;
  toolbelt::Logger logger_;

  static constexpr size_t kMaxEvents = 16384;
  std::list<std::shared_ptr<stagezero::Event>> event_cache_;
  size_t num_cached_events_ = 0;
};
}  // namespace stagezero::flight
