// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "flight/flight_director.h"
#include "toolbelt/hexdump.h"
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
      capcom_addr_(capcom_addr), root_dir_(root_dir), notify_fd_(notify_fd),
      capcom_client_(stagezero::capcom::client::ClientMode::kNonBlocking),
      autostart_capcom_client_(
          stagezero::capcom::client::ClientMode::kNonBlocking) {}

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
  // We have two capcom clients, one for autostart subsystems that need to
  // continue running, and the other for regular subsystems.
  if (absl::Status status =
          autostart_capcom_client_.Init(capcom_addr_, "FlightDirector");
      !status.ok()) {
    return status;
  }

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

  // The Event Monitor coroutine takes incoming events from Capcom and forwards
  // them to all clients.
  coroutines_.insert(std::make_unique<co::Coroutine>(
      co_scheduler_, [this](co::Coroutine *c) { EventMonitorCoroutine(c); },
      "Event Monitor"));

  // Add all subsystems and computes to capcom.
  coroutines_.insert(
      std::make_unique<co::Coroutine>(co_scheduler_, [this](co::Coroutine *c) {
        for (auto & [ name, compute ] : computes_) {
          if (absl::Status status = RegisterCompute(compute, c); !status.ok()) {
            std::cerr << "Failed to register compute " << name << ": "
                      << status.ToString() << std::endl;
          }
        }
        for (auto &var : global_variables_) {
          if (absl::Status status = RegisterGlobalVariable(var, c);
              !status.ok()) {
            std::cerr << "Failed to register global variable " << var.name
                      << ": " << status.ToString() << std::endl;
          }
        }
        for (auto & [ name, subsystem ] : interfaces_) {
          if (absl::Status status = RegisterSubsystemGraph(subsystem, c);
              !status.ok()) {
            std::cerr << "Failed to register interface " << name << ": "
                      << status.ToString() << std::endl;
          }
        }
        for (auto & [ name, subsystem ] : autostarts_) {
          if (absl::Status status = AutostartSubsystem(subsystem, c);
              !status.ok()) {
            std::cerr << "Failed to autostart subsystem " << name << ": "
                      << status.ToString() << std::endl;
          }
        }
      }));

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
  process->startup_timeout_secs = options.has_startup_timeout_secs()
                                      ? options.startup_timeout_secs()
                                      : kDefaultStartupTimeout;
  process->sigint_shutdown_timeout_secs =
      options.has_sigint_shutdown_timeout_secs()
          ? options.sigint_shutdown_timeout_secs()
          : kDefaultSigIntTimeout;
  process->sigterm_shutdown_timeout_secs =
      options.has_sigterm_shutdown_timeout_secs()
          ? options.sigterm_shutdown_timeout_secs()
          : kDefaultSigTermTimeout;
  process->notify = options.has_notify() ? options.notify() : true;
  process->user = options.user();
  process->group = options.group();
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
  process->startup_timeout_secs = options.has_startup_timeout_secs()
                                      ? options.startup_timeout_secs()
                                      : kDefaultStartupTimeout;
  process->sigint_shutdown_timeout_secs =
      options.has_sigint_shutdown_timeout_secs()
          ? options.sigint_shutdown_timeout_secs()
          : kDefaultSigIntTimeout;
  process->sigterm_shutdown_timeout_secs =
      options.has_sigterm_shutdown_timeout_secs()
          ? options.sigterm_shutdown_timeout_secs()
          : kDefaultSigTermTimeout;
  process->user = options.user();
  process->group = options.group();
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

static void ParseStream(const std::string &process_name,
                        const stagezero::flight::proto::Stream &stream, int fd,
                        std::vector<Stream> *vec) {
  Stream s;

  switch (stream.where()) {
  case flight::proto::Stream::STAGEZERO:
    s.disposition = Stream::Disposition::kStageZero;
    break;
  case flight::proto::Stream::LOGGER:
    s.disposition = Stream::Disposition::kLog;
    break;
  case flight::proto::Stream::FILE: {
    std::string filename = stream.filename();
    s.disposition = Stream::Disposition::kFile;
    if (filename.empty()) {
      filename = absl::StrFormat("/tmp/%s.%d.log", process_name, fd);
    }
    s.data = filename;
  } break;
  case flight::proto::Stream::CLOSE:
    s.disposition = Stream::Disposition::kClose;
    break;
  default:
    return;
  }
  // If the stdin is missing, omit it from the output vector.  There's
  // no meaning to redirecting stdin from the logger, but omitting it
  // will select the default value, which is kLog.
  if (fd == STDIN_FILENO && s.disposition == Stream::Disposition::kLog) {
    return;
  }
  if (fd == STDIN_FILENO) {
    s.direction = Stream::Direction::kInput;
  } else {
    s.direction = Stream::Direction::kOutput;
  }
  s.stream_fd = fd;
  vec->push_back(s);
}

absl::Status FlightDirector::LoadSubsystemGraph(
    std::unique_ptr<proto::SubsystemGraph> graph) {
  // Load the computes.
  for (auto &c : graph->compute()) {
    const Compute *c2 = FindCompute(c.name());
    if (c2 != nullptr) {
      return absl::InternalError(
          absl::StrFormat("Duplicate compute %s", c.name()));
    }
    Compute compute = {c.name(), toolbelt::InetAddress(c.ip_addr(), c.port())};

    AddCompute(c.name(), std::move(compute));
  }

  // Global variables.
  for (auto &v : graph->var()) {
    Variable var = {
        .name = v.name(), .value = v.value(), .exported = v.exported()};

    AddGlobalVariable(std::move(var));
  }

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

    // If we have any modules, add a dependency to subspace, if it's not already
    // there.  And only if there's a subsystem called "subspace".
    if (!s.module().empty()) {
      Subsystem *subspace = FindSubsystem("subspace");
      if (subspace != nullptr) {
        if (std::find(subsystem->deps.begin(), subsystem->deps.end(),
                      subspace) == subsystem->deps.end()) {
          subsystem->deps.push_back(subspace);
        }
      }
    }

    for (auto &arg : s.arg()) {
      subsystem->args.push_back(arg);
    }
    for (auto &var : s.var()) {
      subsystem->vars.push_back({.name = var.name(),
                                 .value = var.value(),
                                 .exported = var.exported()});
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
      process->compute = proc.compute();

      auto &options = proc.options();
      ParseProcessOptions(process.get(), options);
      ParseStream(process->name, proc.stdin(), STDIN_FILENO, &process->streams);
      ParseStream(process->name, proc.stdout(), STDOUT_FILENO,
                  &process->streams);
      ParseStream(process->name, proc.stderr(), STDERR_FILENO,
                  &process->streams);
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
      zygote->compute = z.compute();

      auto &options = z.options();
      ParseProcessOptions(zygote.get(), options);
      ParseStream(zygote->name, z.stdin(), STDIN_FILENO, &zygote->streams);
      ParseStream(zygote->name, z.stdout(), STDOUT_FILENO, &zygote->streams);
      ParseStream(zygote->name, z.stderr(), STDERR_FILENO, &zygote->streams);
      subsystem->processes.push_back(std::move(zygote));
    }

    // And modules.
    for (auto &mod : s.module()) {
      if (!CheckProcessUniqueness(*subsystem, mod.name())) {
        return absl::InternalError(absl::StrFormat(
            "Module %s already exists in subsystem %s", mod.name(), s.name()));
      }
      Subsystem *zygote = FindSubsystem(mod.zygote());
      if (zygote == nullptr) {
        return absl::InternalError(
            absl::StrFormat("Module %s refers to nonexistent zygote %s",
                            mod.name(), mod.zygote()));
      }
      auto module = std::make_unique<Module>();
      module->name = mod.name();
      module->zygote = mod.zygote();
      module->dso = mod.dso();
      module->compute = mod.compute();
      ParseStream(module->name, mod.stdin(), STDIN_FILENO, &module->streams);
      ParseStream(module->name, mod.stdout(), STDOUT_FILENO, &module->streams);
      ParseStream(module->name, mod.stderr(), STDERR_FILENO, &module->streams);

      // Automatically add a dep to the zygote unless it's already there.
      bool present = false;
      for (auto *dep : subsystem->deps) {
        if (dep->name == module->zygote) {
          present = true;
          break;
        }
      }
      if (!present) {
        subsystem->deps.push_back(zygote);
      }
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
    absl::StrAppend(&path, "->", subsystem->name);
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
  return absl::OkStatus();
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

absl::Status FlightDirector::RegisterCompute(const Compute &compute,
                                             co::Coroutine *c) {
  return capcom_client_.AddCompute(compute.name, compute.addr, c);
}

absl::Status FlightDirector::RegisterGlobalVariable(const Variable &var,
                                                    co::Coroutine *c) {
  return capcom_client_.AddGlobalVariable(var, c);
}

absl::Status FlightDirector::RegisterSubsystemGraph(Subsystem *root,
                                                    co::Coroutine *c) {
  std::vector<Subsystem *> flattened_graph = FlattenSubsystemGraph(root);
  for (auto &subsystem : flattened_graph) {
    capcom::client::SubsystemOptions options;
    for (auto &proc : subsystem->processes) {
      switch (proc->Type()) {
      case ProcessType::kStatic: {
        StaticProcess *src = static_cast<StaticProcess *>(proc.get());
        options.static_processes.push_back({
            .name = src->name,
            .description = src->description,
            .executable = src->executable,
            .compute = src->compute,
            .vars = src->vars,
            .args = src->args,
            .startup_timeout_secs = src->startup_timeout_secs,
            .sigint_shutdown_timeout_secs = src->sigint_shutdown_timeout_secs,
            .sigterm_shutdown_timeout_secs = src->sigterm_shutdown_timeout_secs,
            .notify = src->notify,
            .streams = src->streams,
            .user = src->user,
            .group = src->group,
        });
        break;
      }
      case ProcessType::kZygote: {
        Zygote *src = static_cast<Zygote *>(proc.get());
        options.zygotes.push_back({
            .name = src->name,
            .description = src->description,
            .executable = src->executable,
            .compute = src->compute,
            .vars = src->vars,
            .args = src->args,
            .startup_timeout_secs = src->startup_timeout_secs,
            .sigint_shutdown_timeout_secs = src->sigint_shutdown_timeout_secs,
            .sigterm_shutdown_timeout_secs = src->sigterm_shutdown_timeout_secs,
            .streams = src->streams,
            .user = src->user,
            .group = src->group,
        });
        break;
      }
      case ProcessType::kModule: {
        Module *src = static_cast<Module *>(proc.get());
        options.virtual_processes.push_back({
            .name = src->name,
            .description = src->description,
            .zygote = src->zygote,
            .dso = src->dso,
            .main_func = "ModuleMain",
            .compute = src->compute,
            .vars = src->vars,
            .startup_timeout_secs = src->startup_timeout_secs,
            .sigint_shutdown_timeout_secs = src->sigint_shutdown_timeout_secs,
            .sigterm_shutdown_timeout_secs = src->sigterm_shutdown_timeout_secs,
            .streams = src->streams,
            .user = src->user,
            .group = src->group,
        });
        // Add the args, but after the two module args: name and
        // subspace_socket.
        auto &vproc = options.virtual_processes.back();
        vproc.args.push_back(src->name);
        vproc.args.push_back("${subspace_socket}");
        for (auto &arg : src->args) {
          vproc.args.push_back(arg);
        }
        break;
      }
      }
    }

    // Add deps as children.
    for (auto &dep : subsystem->deps) {
      options.children.push_back(dep->name);
    }

    // Add subsytem vars and args.
    for (auto &arg : subsystem->args) {
      options.args.push_back(arg);
    }
    for (auto &var : subsystem->vars) {
      options.vars.push_back(var);
    }

    absl::Status status =
        capcom_client_.AddSubsystem(subsystem->name, std::move(options), c);
    if (!status.ok()) {
      return status;
    }
  }

  return absl::OkStatus();
}

absl::Status FlightDirector::AutostartSubsystem(Subsystem *subsystem,
                                                co::Coroutine *c) {
  return autostart_capcom_client_.StartSubsystem(subsystem->name, c);
}

void FlightDirector::EventMonitorCoroutine(co::Coroutine *c) {
  for (;;) {
    absl::StatusOr<std::shared_ptr<stagezero::Event>> event =
        capcom_client_.WaitForEvent(c);
    if (!event.ok()) {
      logger_.Log(toolbelt::LogLevel::kError, "Failed to read capcom event: %s",
                  event.status().ToString().c_str());
      // TODO: what do we do here?
      return;
    }
    auto proto_event = std::make_shared<stagezero::proto::Event>();
    (*event)->ToProto(proto_event.get());
    for (auto &handler : client_handlers_) {
      if (absl::Status status = handler->QueueEvent(proto_event);
          !status.ok()) {
        logger_.Log(toolbelt::LogLevel::kError, "Failed to queue event: %s",
                    status.ToString().c_str());
      }
    }
  }
}
} // namespace stagezero::flight
