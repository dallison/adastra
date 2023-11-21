// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "stagezero/process.h"
#include "stagezero/symbols.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"

#include <memory>
#include <string>

namespace stagezero {

constexpr int64_t kReady = 1;
constexpr int64_t kStopped = 2;

class ClientHandler;

class StageZero {
public:
  StageZero(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
            int notify_fd = -1);
  ~StageZero();
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

  bool AddProcess(std::string id, std::shared_ptr<Process> process) {
    std::cerr << "adding process " << id << std::endl;
    auto[it, inserted] =
        processes_.emplace(std::make_pair(std::move(id), std::move(process)));
    return inserted;
  }

  bool AddZygote(std::string name, std::string id,
                 std::shared_ptr<Zygote> zygote) {
    bool p_inserted = AddProcess(id, zygote);
    if (!p_inserted) {
      std::cerr << "process " << id << " already exists" << std::endl;
      return false;
    }

    std::cerr << "adding zygote " << name << std::endl;
    auto[it, z_inserted] = zygotes_.emplace(name, zygote);
    if (!z_inserted) {
      std::cerr << "zygote " << name << " already exists" << std::endl;
    }
    return z_inserted;
  }

  absl::Status RemoveProcess(Process *proc) {
    std::cerr << "Removing process " << proc->Name() << std::endl;
    if (proc->IsZygote()) {
      auto it = zygotes_.find(proc->Name());
      if (it == zygotes_.end()) {
        return absl::InternalError(
            absl::StrFormat("No such zygote %s", proc->Name()));
      }
      zygotes_.erase(it);
    }

    const std::string &id = proc->GetId();
    auto it = processes_.find(id);
    if (it == processes_.end()) {
      return absl::InternalError(absl::StrFormat("No such process %s", id));
    }
    processes_.erase(it);
    return absl::OkStatus();
  }

  void TryRemoveProcess(std::shared_ptr<Process> proc) {
    if (proc->IsZygote()) {
      auto it = zygotes_.find(proc->Name());
      if (it != zygotes_.end()) {
        zygotes_.erase(it);
      }
    }

    const std::string &id = proc->GetId();
    auto it = processes_.find(id);
    if (it != processes_.end()) {
      processes_.erase(it);
    }
  }

  std::shared_ptr<Process> FindProcess(const std::string &id) {
    auto it = processes_.find(id);
    if (it == processes_.end()) {
      return nullptr;
    }
    return it->second;
  }

  std::shared_ptr<Zygote> FindZygote(const std::string &id) {
    auto it = zygotes_.find(id);
    if (it == zygotes_.end()) {
      return nullptr;
    }
    return it->second;
  }

  void KillAllProcesses();
  void KillAllProcesses(co::Coroutine* c);

  co::CoroutineScheduler &co_scheduler_;
  toolbelt::InetAddress addr_;
  toolbelt::FileDescriptor notify_fd_;
  std::string compute_;
  
  // All coroutines are owned by this set.
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> coroutines_;

  std::vector<std::shared_ptr<ClientHandler>> client_handlers_;
  bool running_ = false;
  absl::flat_hash_map<std::string, std::shared_ptr<Process>> processes_;

  absl::flat_hash_map<std::string, std::shared_ptr<Zygote>> zygotes_;
  toolbelt::Logger logger_;

  SymbolTable global_symbols_;
};

} // namespace stagezero
