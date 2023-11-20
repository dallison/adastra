// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/stagezero.h"
#include "absl/strings/str_format.h"
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

namespace stagezero {

StageZero::StageZero(co::CoroutineScheduler &scheduler,
                     toolbelt::InetAddress addr, int notify_fd)
    : co_scheduler_(scheduler), addr_(addr), notify_fd_(notify_fd) {}

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
    return ".";
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
  co_scheduler_.SetCompletionCallback([this](co::Coroutine *c) {
    coroutines_.erase(c);
  });

  // Start the listener coroutine.
  coroutines_.insert(
      std::make_unique<co::Coroutine>(co_scheduler_,
                                      [this, &listen_socket](co::Coroutine *c) {
                                        ListenerCoroutine(listen_socket, c);
                                      },
                                      "Listener Socket"));

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

void StageZero::KillAllProcesses(co::Coroutine *c) {
  // Copy all processes out of the processes_ map as we will
  // be removing them as they are killed.
  std::vector<std::shared_ptr<Process>> procs;

  for (auto & [ id, proc ] : processes_) {
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
}
} // namespace stagezero
