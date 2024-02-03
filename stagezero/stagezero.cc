// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/stagezero.h"
#include "absl/strings/str_format.h"
#include "stagezero/cgroup.h"
#include "stagezero/client_handler.h"
#include "toolbelt/sockets.h"
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
// For _NSGetExecutablePath
#include <mach-o/dyld.h>
#endif

namespace adastra::stagezero {

StageZero::StageZero(co::CoroutineScheduler &scheduler,
                     toolbelt::InetAddress addr, bool log_to_output,
                     const std::string &logdir, const std::string &log_level,
                     int notify_fd)
    : co_scheduler_(scheduler), addr_(addr), notify_fd_(notify_fd),
      logger_("stagezero", log_to_output) {
  logger_.SetLogLevel(log_level);
  // Add a global symbol for where we want log files.
  global_symbols_.AddSymbol("logdir", logdir, false);
}

StageZero::~StageZero() {
  // Clear this before other data members get destroyed.
  client_handlers_.clear();
}

void StageZero::Stop() { co_scheduler_.Stop(); }

void StageZero::CloseHandler(std::shared_ptr<ClientHandler> handler) {
  for (auto it = client_handlers_.begin(); it != client_handlers_.end(); it++) {
    if (*it == handler) {
      client_handlers_.erase(it);
      return;
    }
  }
}

absl::Status
StageZero::HandleIncomingConnection(toolbelt::TCPSocket &listen_socket,
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
      co_scheduler_, [ this, handler = std::move(handler) ](co::Coroutine * c) {

        handler->Run(c);
        handler->KillAllProcesses();
        CloseHandler(handler);
      },
      "Client handler"));

  return absl::OkStatus();
}

// This coroutine listens for incoming client connections on the given
// socket and spawns a handler coroutine to handle the communication with
// the client.
void StageZero::ListenerCoroutine(toolbelt::TCPSocket &listen_socket,
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

std::string GetRunfilesDir() {
  // Look for a directory with the suffix ".runfiles".
  char wd[PATH_MAX];
  char *p = getcwd(wd, sizeof(wd) - 1);
  if (p == nullptr) {
    return ".";
  }
  wd[sizeof(wd) - 1] = '\0';
  char *s = strstr(p, ".runfiles");
  if (s == nullptr) {
    // No .runfiles in current directory.  Let's use the executable
    // name and append .runfiles to see if that is a valid directory.
    char path[PATH_MAX];

#if defined(__APPLE__)
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) {
      return ".";
    }
#elif defined(__linux__)
    int e = readlink("/proc/self/exe", path, sizeof(path));
    if (e == -1) {
      return ".";
    }
#else
#error "Unsupported OS"
#endif
    // Check for a valid directory.
    std::string dir = absl::StrFormat("%s.runfiles", path);
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) {
      if (st.st_mode & S_IFDIR) {
        return dir;
      }
    }
    // No .runfiles at end of path, look for a .runfiles directory.
    s = strstr(path, ".runfiles");
    if (s == nullptr) {
      return ".";
    }
  }
  // Move forward to the next / or EOS.
  while (*s != '\0' && *s != '/') {
    s++;
  }
  // Overwrite terminating char with EOS.
  *s = '\0';
  return p;
}

absl::Status StageZero::Run() {
  // Work out the runfiles directory and set a variable
  char *runfiles = getenv("RUNFILES_DIR");
  std::string runfiles_dir;
  if (runfiles == nullptr) {
    runfiles_dir = GetRunfilesDir();
  } else {
    runfiles_dir = runfiles;
  }
  global_symbols_.AddSymbol("runfiles_dir", runfiles_dir, false);

  logger_.Log(toolbelt::LogLevel::kInfo, "StageZero running on address %s",
              addr_.ToString().c_str());
  toolbelt::TCPSocket listen_socket;

  if (absl::Status status = listen_socket.SetCloseOnExec(); !status.ok()) {
    return status;
  }

  if (absl::Status status = listen_socket.SetReuseAddr(); !status.ok()) {
    return status;
  }

  if (absl::Status status = listen_socket.SetReusePort(); !status.ok()) {
    return status;
  }

  if (absl::Status status = listen_socket.Bind(addr_, true); !status.ok()) {
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

  // Start a new process group.
  setpgrp();

  // Run the coroutine main loop.
  co_scheduler_.Run();

  KillAllProcesses();

  // Notify that we are stopped.
  if (notify_fd_.Valid()) {
    int64_t val = kStopped;
    (void)::write(notify_fd_.Fd(), &val, 8);
  }

  return absl::OkStatus();
}

void StageZero::KillAllProcesses() {
  logger_.Log(toolbelt::LogLevel::kInfo,
              "Killing all processes on StageZero exit");
  // Copy all processes out of the processes_ map as we will
  // be removing them as they are killed.
  std::vector<std::shared_ptr<Process>> procs;

  for (auto & [ id, proc ] : processes_) {
    procs.push_back(proc);
  }
  for (auto &proc : procs) {
    proc->KillNow();
  }
}

void StageZero::KillAllProcesses(bool emergency, co::Coroutine *c) {
  // Copy all processes out of the processes_ map as we will
  // be removing them as they are killed.
  std::vector<std::shared_ptr<Process>> procs;

  for (auto & [ id, proc ] : processes_) {
    if (!emergency && proc->IsCritical()) {
      continue;
    }
    procs.push_back(proc);
  }
  for (auto &proc : procs) {
    proc->KillNow();
  }

  // Wait for all the processes to stop.
  for (;;) {
    bool all_dead = true;
    for (auto &proc : procs) {
      if (proc->IsRunning()) {
        all_dead = false;
        break;
      }
    }
    if (all_dead) {
      break;
    }
    c->Millisleep(100);
  }

  if (emergency) {
    AddCoroutine(std::make_unique<co::Coroutine>(
        co_scheduler_, [this](co::Coroutine *c2) {
          // An emergency abort also stops StageZero.
          c2->Sleep(1);
          logger_.Log(toolbelt::LogLevel::kFatal, "Emergency abort");
        }));
  }
}

absl::Status StageZero::RegisterCgroup(const Cgroup &cgroup) {
  return adastra::stagezero::CreateCgroup(cgroup, logger_);
}

absl::Status StageZero::UnregisterCgroup(const std::string &cgroup) {
  return adastra::stagezero::RemoveCgroup(cgroup, logger_);
}

absl::Status StageZero::SendProcessStartEvent(const std::string &process_id) {
  for (auto &client : client_handlers_) {
    (void)client->SendProcessStartEvent(process_id);
  }
  return absl::OkStatus();
}

absl::Status StageZero::SendProcessStopEvent(const std::string &process_id,
                                             bool exited, int exit_status,
                                             int term_signal) {
  for (auto &client : client_handlers_) {
    (void)client->SendProcessStopEvent(process_id, exited, exit_status, term_signal);
  }
  return absl::OkStatus();
}
} // namespace adastra::stagezero
