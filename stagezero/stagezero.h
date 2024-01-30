// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "stagezero/process.h"
#include "stagezero/symbols.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "common/cgroup.h"

#include <memory>
#include <string>

namespace adastra::stagezero {

constexpr int64_t kReady = 1;
constexpr int64_t kStopped = 2;

class ClientHandler;

class StageZero {
public:
  StageZero(co::CoroutineScheduler &scheduler, toolbelt::InetAddress addr,
            bool log_to_output, const std::string &logdir, const std::string& log_level = "debug", 
            int notify_fd = -1);
  ~StageZero();
  absl::Status Run();
  void Stop();

  void ShowCoroutines() { co_scheduler_.Show(); }

private:
  friend class ClientHandler;
  friend class Zygote;
  friend class VirtualProcess;
  absl::Status HandleIncomingConnection(toolbelt::TCPSocket &listen_socket,
                                        co::Coroutine *c);

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) {
    coroutines_.insert(std::move(c));
  }

  void CloseHandler(std::shared_ptr<ClientHandler> handler);
  void ListenerCoroutine(toolbelt::TCPSocket &listen_socket, co::Coroutine *c);

  bool AddProcess(std::string id, std::shared_ptr<Process> process) {
    auto[it, inserted] =
        processes_.emplace(std::make_pair(std::move(id), std::move(process)));
    return inserted;
  }

  bool AddZygote(std::string name, std::string id,
                 std::shared_ptr<Zygote> zygote) {
    bool p_inserted = AddProcess(id, zygote);
    if (!p_inserted) {
      return false;
    }

    auto[it, z_inserted] = zygotes_.emplace(name, zygote);
    return z_inserted;
  }

  bool AddVirtualProcess(int pid, std::shared_ptr<Process> proc) {
    auto[it, inserted] = virtual_processes_.emplace(pid, proc);
    return inserted;
  }

  absl::Status RemoveProcess(Process *proc) {
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
    } else if (proc->IsVirtual()) {
      auto it = virtual_processes_.find(proc->GetPid());
      if (it != virtual_processes_.end()) {
        virtual_processes_.erase(it);
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

  std::shared_ptr<Process> FindVirtualProcess(int pid) {
    auto it = virtual_processes_.find(pid);
    if (it == virtual_processes_.end()) {
      return nullptr;
    }
    return it->second;
  }

  absl::Status RemoveVirtualProcess(int pid) {
    auto it = virtual_processes_.find(pid);
    if (it == virtual_processes_.end()) {
      return absl::InternalError(absl::StrFormat("No such zygote %d", pid));
    }
    virtual_processes_.erase(it);
    return absl::OkStatus();
  }

  std::shared_ptr<Zygote> FindZygote(const std::string &id) {
    auto it = zygotes_.find(id);
    if (it == zygotes_.end()) {
      return nullptr;
    }
    return it->second;
  }

  bool AddCgroup(std::string name, Cgroup cgroup) {
    auto[it, inserted] =
        cgroups_.emplace(std::make_pair(std::move(name), std::move(cgroup)));
    return inserted;
  }

  absl::Status RemoveCgroup(const std::string& cgroup) {
    auto it = cgroups_.find(cgroup);
    if (it == cgroups_.end()) {
      return absl::InternalError(absl::StrFormat("No such cgroup %s", cgroup));
    }
    cgroups_.erase(it);
    return absl::OkStatus();
  }

  Cgroup* FindCgroup(const std::string &name) {
    auto it = cgroups_.find(name);
    if (it == cgroups_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  void KillAllProcesses();
  void KillAllProcesses(bool emergency, co::Coroutine *c);

  absl::Status RegisterCgroup(const Cgroup& cgroup);
  absl::Status UnregisterCgroup(const std::string& cgroup);

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
  absl::flat_hash_map<int, std::shared_ptr<Process>> virtual_processes_;
  toolbelt::Logger logger_;

  absl::flat_hash_map<std::string, Cgroup> cgroups_;

  SymbolTable global_symbols_;
}; // namespace adastra::stagezero

} // namespace adastra::stagezero
