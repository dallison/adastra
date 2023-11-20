// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "flight/flight_director.h"
#include <cassert>
#include <fcntl.h>
#include <fstream>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include <iostream>

namespace stagezero::flight {

FlightDirector::FlightDirector(co::CoroutineScheduler &scheduler,
                               toolbelt::InetAddress addr,
                               toolbelt::InetAddress capcom_addr,
                               const std::string &root_dir, int notify_fd)
    : co_scheduler_(scheduler), addr_(std::move(addr)),
      capcom_addr_(capcom_addr), root_dir_(root_dir), notify_fd_(notify_fd) {}

FlightDirector::~FlightDirector() {
  // Clear this before other data members get destroyed.
  client_handlers_.clear();
}

void FlightDirector::Stop() { co_scheduler_.Stop(); }

void FlightDirector::CloseHandler(std::shared_ptr<ClientHandler> handler) {
  for (auto it = client_handlers_.begin(); it != client_handlers_.end(); it++) {
    if (*it == handler) {
      client_handlers_.erase(it);
      break;
    }
  }
}

absl::Status
FlightDirector::HandleIncomingConnection(toolbelt::TCPSocket &listen_socket,
                                         co::Coroutine *c) {
  absl::StatusOr<toolbelt::TCPSocket> s = listen_socket.Accept(c);
  if (!s.ok()) {
    return s.status();
  }

  std::cout << "client handler address: " << s->BoundAddress().ToString()
            << std::endl;
  if (absl::Status status = s->SetCloseOnExec(); !status.ok()) {
    return status;
  }

  std::shared_ptr<ClientHandler> handler =
      std::make_shared<ClientHandler>(*this, std::move(*s));
  client_handlers_.push_back(handler);

  coroutines_.insert(std::make_unique<co::Coroutine>(
      co_scheduler_,
      [this, handler](co::Coroutine *c) {
        handler->Run(c);
        logger_.Log(toolbelt::LogLevel::kInfo, "client %s closed",
                    handler->GetClientName().c_str());
        CloseHandler(handler);
      },
      "Client handler"));

  return absl::OkStatus();
}

// This coroutine listens for incoming client connections on the given
// socket and spawns a handler coroutine to handle the communication with
// the client.
void FlightDirector::ListenerCoroutine(toolbelt::TCPSocket &listen_socket,
                                       co::Coroutine *c) {
  for (;;) {
    absl::Status status = HandleIncomingConnection(listen_socket, c);
    if (!status.ok()) {
      logger_.Log(toolbelt::LogLevel::kError,
                  "Unable to make incoming connection: %s",
                  status.ToString().c_str());
    }
  }
}

absl::Status FlightDirector::Run() {
  toolbelt::TCPSocket listen_socket;

  if (absl::Status status = listen_socket.SetReuseAddr(); !status.ok()) {
    return status;
  }

  if (absl::Status status = listen_socket.SetReusePort(); !status.ok()) {
    return status;
  }

  if (absl::Status status = listen_socket.Bind(addr_, true); !status.ok()) {
    return status;
  }

  // Connect to capcom.
  if (absl::Status status = capcom_client_.Init(capcom_addr_, "FlightDirector");
      !status.ok()) {
    return status;
  }

  if (absl::Status status =
          LoadAllSubsystemGraphs(std::filesystem::path(root_dir_));
      !status.ok()) {
    return status;
  }

  if (absl::Status status = CheckForSubsystemLoops(); !status.ok()) {
    return status;
  }

  // Notify listener that we are ready.
  if (notify_fd_.Valid()) {
    int64_t val = kReady;
    (void)::write(notify_fd_.Fd(), &val, 8);
  }

  // Register a callback to be called when a coroutine completes.  The
  // server keeps track of all coroutines created.
  // This deletes them when they are done.
  co_scheduler_.SetCompletionCallback(
      [this](co::Coroutine *c) { coroutines_.erase(c); });

  // Start the listener coroutine.
  coroutines_.insert(
      std::make_unique<co::Coroutine>(co_scheduler_,
                                      [this, &listen_socket](co::Coroutine *c) {
                                        ListenerCoroutine(listen_socket, c);
                                      },
                                      "Listener Socket"));

  // Run the coroutine main loop.
  co_scheduler_.Run();

  // Notify that we are stopped.
  if (notify_fd_.Valid()) {
    int64_t val = kStopped;
    (void)::write(notify_fd_.Fd(), &val, 8);
  }

  return absl::OkStatus();
}

absl::Status
FlightDirector::LoadAllSubsystemGraphs(const std::filesystem::path &dir) {
  std::vector<std::unique_ptr<proto::SubsystemGraph>> graphs;
  if (absl::Status status = LoadAllSubsystemGraphsFromDir(dir, graphs);
      !status.ok()) {
    return status;
  }

  for (auto &graph : graphs) {
    if (absl::Status status = LoadSubsystemGraph(std::move(graph));
        !status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

absl::Status FlightDirector::LoadAllSubsystemGraphsFromDir(
    const std::filesystem::path &dir,
    std::vector<std::unique_ptr<proto::SubsystemGraph>> &graphs) {
  for (auto &file : std::filesystem::directory_iterator(dir)) {
    if (std::filesystem::is_directory(file)) {
      if (absl::Status status = LoadAllSubsystemGraphsFromDir(file, graphs);
          !status.ok()) {
        return status;
      }
    } else {
      absl::StatusOr<std::unique_ptr<proto::SubsystemGraph>> graph =
          PreloadSubsystemGraph(file);
      if (!graph.ok()) {
        return graph.status();
      }
      graphs.push_back(std::move(*graph));
    }
  }

  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<proto::SubsystemGraph>>
FlightDirector::PreloadSubsystemGraph(const std::filesystem::path &file) {
  toolbelt::FileDescriptor fd(open(file.c_str(), O_RDONLY));
  if (!fd.Valid()) {
    return absl::InternalError(
        absl::StrFormat("Failed to open file %s: %s", file, strerror(errno)));
  }

  google::protobuf::io::FileInputStream in(fd.Fd());

  auto graph = std::make_unique<flight::proto::SubsystemGraph>();
  if (!google::protobuf::TextFormat::Parse(&in, graph.get())) {
    return absl::InternalError(
        absl::StrFormat("Failed to parse subsystem graph from %s", file));
  }

  for (auto &subsystem : graph->subsystem()) {
    if (subsystems_.find(subsystem.name()) != subsystems_.end()) {
      return absl::InternalError(
          absl::StrFormat("Duplicate subsystem %s", subsystem.name()));
    }
    auto s = std::make_unique<Subsystem>();
    s->name = subsystem.name();
    subsystems_.emplace(subsystem.name(), std::move(s));
  }
  return graph;
}

static void ParseProcessOptions(Process *process,
                                const proto::ProcessOptions &options) {
  for (auto &var : options.vars()) {
    process->vars.push_back(
        {.name = var.name(), .value = var.value(), .exported = var.exported()});
  }
  for (auto &arg : options.args()) {
    process->args.push_back(arg);
  }
  process->startup_timeout_secs = options.startup_timeout_secs();
  process->sigint_shutdown_timeout_secs =
      options.sigint_shutdown_timeout_secs();
  process->sigterm_shutdown_timeout_secs =
      options.sigterm_shutdown_timeout_secs();
  process->notify = options.notify();
}

static void ParseModuleOptions(Process *process,
                               const proto::ModuleOptions &options) {
  for (auto &var : options.vars()) {
    process->vars.push_back(
        {.name = var.name(), .value = var.value(), .exported = var.exported()});
  }
  for (auto &arg : options.args()) {
    process->args.push_back(arg);
  }
  process->startup_timeout_secs = options.startup_timeout_secs();
  process->sigint_shutdown_timeout_secs =
      options.sigint_shutdown_timeout_secs();
  process->sigterm_shutdown_timeout_secs =
      options.sigterm_shutdown_timeout_secs();
}

static bool CheckProcessUniqueness(const Subsystem &subsystem,
                                   const std::string &name) {
  for (auto &proc : subsystem.processes) {
    if (proc->name == name) {
      return false;
    }
  }
  return true;
}

absl::Status FlightDirector::LoadSubsystemGraph(
    std::unique_ptr<proto::SubsystemGraph> graph) {
  for (auto &s : graph->subsystem()) {
    Subsystem *subsystem = FindSubsystem(s.name());
    assert(subsystem != nullptr);

    // Link all the dependency subsystems.
    for (auto &name : s.dep()) {
      Subsystem *dep = FindSubsystem(name);
      if (dep == nullptr) {
        return absl::InternalError(absl::StrFormat(
            "No such subsystem %s (dep of %s)", name, s.name()));
      }
      // Check that dep is unique.
      for (auto &sd : subsystem->deps) {
        if (sd->name == name) {
          return absl::InternalError(absl::StrFormat(
              "Subsystem %s is already a dep of %s", name, s.name()));
        }
      }
      subsystem->deps.push_back(dep);
    }

    // Load all the processes in the subsystem.
    // First static processes.
    for (auto &proc : s.static_process()) {
      if (!CheckProcessUniqueness(*subsystem, proc.name())) {
        return absl::InternalError(
            absl::StrFormat("Process %s already exists in subsystem %s",
                            proc.name(), s.name()));
      }
      auto process = std::make_unique<StaticProcess>();
      process->name = proc.name();
      process->executable = proc.executable();

      auto &options = proc.options();
      ParseProcessOptions(process.get(), options);
      subsystem->processes.push_back(std::move(process));
    }

    // Now zygotes.
    for (auto &z : s.zygote()) {
      if (!CheckProcessUniqueness(*subsystem, z.name())) {
        return absl::InternalError(absl::StrFormat(
            "Zygote %s already exists in subsystem %s", z.name(), s.name()));
      }
      auto zygote = std::make_unique<Zygote>();
      zygote->name = z.name();
      zygote->executable = z.executable();

      auto &options = z.options();
      ParseProcessOptions(zygote.get(), options);
      subsystem->processes.push_back(std::move(zygote));
    }

    // And modules.
    for (auto &mod : s.module()) {
      if (!CheckProcessUniqueness(*subsystem, mod.name())) {
        return absl::InternalError(absl::StrFormat(
            "Module %s already exists in subsystem %s", mod.name(), s.name()));
      }
      auto module = std::make_unique<Module>();
      module->name = mod.name();
      module->dso = mod.dso();

      auto &options = mod.options();
      ParseModuleOptions(module.get(), options);
      subsystem->processes.push_back(std::move(module));
    }
  }

  // Interfaces.
  for (auto &iface : graph->interface()) {
    Subsystem *subsystem = FindSubsystem(iface);
    if (subsystem == nullptr) {
      return absl::InternalError(
          absl::StrFormat("No such subsystem %s used as interface", iface));
    }
    if (interfaces_.find(iface) != interfaces_.end()) {
      return absl::InternalError(
          absl::StrFormat("Subsystem %s is already an interface", iface));
    }
    interfaces_.emplace(iface, subsystem);
  }

  // Autostarts;
  for (auto &name : graph->autostart()) {
    Subsystem *subsystem = FindSubsystem(name);
    if (subsystem == nullptr) {
      return absl::InternalError(
          absl::StrFormat("No such subsystem %s specified as autostart", name));
    }
    if (autostarts_.find(name) != interfaces_.end()) {
      return absl::InternalError(
          absl::StrFormat("Subsystem %s is already autostarted", name));
    }
    autostarts_.emplace(name, subsystem);
  }

  return absl::OkStatus();
}

absl::Status FlightDirector::CheckForSubsystemLoopsRecurse(
    absl::flat_hash_set<Subsystem *> &visited, Subsystem *subsystem,
    std::string path) {
  if (visited.contains(subsystem)) {
    return absl::InternalError(
        absl::StrFormat("Dependency loop in subsystem graph: %s", path));
  }
  visited.insert(subsystem);
  if (path.empty()) {
    path = subsystem->name;
  } else {
    absl::StrAppend(&path, "->", path);
  }
  for (auto *dep : subsystem->deps) {
    if (absl::Status status = CheckForSubsystemLoopsRecurse(visited, dep, path);
        !status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}

absl::Status FlightDirector::CheckForSubsystemLoops() {
  for (auto & [ name, subsystem ] : subsystems_) {
    absl::flat_hash_set<Subsystem *> visited;
    return CheckForSubsystemLoopsRecurse(visited, subsystem.get(), "");
  }
}

std::vector<Subsystem *>
FlightDirector::FlattenSubsystemGraph(Subsystem *root) {
  absl::flat_hash_set<Subsystem *> visited;
  std::vector<Subsystem *> result;
  FlattenSubsystemGraphRecurse(visited, root, result);
  return result;
}

void FlightDirector::FlattenSubsystemGraphRecurse(
    absl::flat_hash_set<Subsystem *> &visited, Subsystem *subsystem,
    std::vector<Subsystem *> &vec) {
  if (visited.contains(subsystem)) {
    return;
  }
  visited.insert(subsystem);
  for (auto &dep : subsystem->deps) {
    FlattenSubsystemGraphRecurse(visited, dep, vec);
  }
  vec.push_back(subsystem);
}

static void BuildCapcom absl::Status
FlightDirector::AddSubsystemGraph(Subsystem *root) {
  std::vector<Subsystem *> flattened_graph = FlattenSubsystemGraph(root);
  for (auto &subsystem : flattened_graph) {
    capcom::client::SubsystemOptions options;
    for (auto &proc : subsystem->processes) {
      switch (proc->Type()) {
      case ProcessType::kStatic: {
        StaticProcess *sproc = static_cast<StaticProcess *>(proc.get());

        options.static_processes.push_back({
          .name = sproc->name, .executable = sproc->executable;

        });
        break;
      }
      case ProcessType::kZygote: {
        break;
      }
      case ProcessType::kModule: {
        break;
      }
      }
    }
    absl::Status status = capcom_client_.AddSubsystem(
        subsystem->name,
        {.static_processes = {
             {
                 .name = "loop1",
                 .executable = "${runfiles_dir}/__main__/testdata/loop",
                 .compute = "localhost1",
             },
             {
                 .name = "loop2",
                 .executable = "${runfiles_dir}/__main__/testdata/loop",
                 .compute = "localhost2",
             }}});
  }
}

} // namespace stagezero::flight
