// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/zygote/zygote_core.h"

#include "absl/container/flat_hash_set.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "toolbelt/fd.h"
#include "toolbelt/hexdump.h"

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include <dlfcn.h>
#include <iostream>
#include <stdlib.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" const char **environ;

ABSL_FLAG(std::string, run, "", "Run this module directly");
ABSL_FLAG(std::string, main, "ModuleMain", "Name of main function");
ABSL_FLAG(std::vector<std::string>, vars, {}, "Variables (name=value) pairs");

namespace stagezero {

ZygoteCore::ZygoteCore(int argc, char **argv) : logger_("zygote_core") {
  absl::ParseCommandLine(argc, argv);

  for (int i = 0; i < argc; i++) {
    args_.push_back(argv[i]);
  }
}

absl::Status ZygoteCore::Run() {
  char *notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd;
    bool ok = absl::SimpleAtoi(notify, &notify_fd);
    if (ok) {
      int64_t val = 1;
      (void)write(notify_fd, &val, 8);
    }
    notification_pipe_.SetFd(notify_fd);
  }

  std::string module_to_run = absl::GetFlag(FLAGS_run);
  if (!module_to_run.empty()) {
    Run(module_to_run, absl::GetFlag(FLAGS_main), absl::GetFlag(FLAGS_vars));
    return absl::OkStatus();
  }

  const char *sname = getenv("STAGEZERO_ZYGOTE_SOCKET_NAME");
  if (sname == nullptr) {
    return absl::InternalError(
        "No STAGEZERO_ZYGOTE_SOCKET_NAME passed to zygote");
  }

  std::string socket_name = sname;
  control_socket_ = std::make_unique<toolbelt::UnixSocket>();
  absl::Status status = control_socket_->Connect(socket_name);
  if (!status.ok()) {
    return absl::InternalError(absl::StrFormat(
        "Zygote failed to connect to control socket: %s", status.ToString()));
  }

  server_ =
      std::make_unique<co::Coroutine>(scheduler_, [this](co::Coroutine *c) {
        while (control_socket_->Connected()) {
          WaitForSpawn(c);
        }

      });

  monitor_ =
      std::make_unique<co::Coroutine>(scheduler_, [this](co::Coroutine *c) {
        constexpr int kWaitTimeMs = 500;

        for (;;) {
          int status;
          pid_t pid = waitpid(-1, &status, WNOHANG);
          if (pid > 0) {
            // A process died.  Write the pid and status to the notification
            // pipe.  Set the top bit to distinguish this from any other
            // notification.
            logger_.Log(toolbelt::LogLevel::kDebug, "Process %d died", pid);
            uint64_t val = (1LL << 63) | (static_cast<uint64_t>(pid) << 32) |
                           static_cast<uint64_t>(status);
            (void)write(notification_pipe_.Fd(), &val, 8);
          }
          c->Millisleep(kWaitTimeMs);
        }

      });

  scheduler_.Run();
  // When we get here we are either in the zygote that has been stopped or
  // we are in the forked process and will invoke the main function.

  if (forked_) {
    // We want to destruct the ZygoteCore as we don't need it in the
    // child process.
    AfterFork a = std::move(after_fork);
    std::string exe = args_[0];
    ZygoteCore::~ZygoteCore();

    InvokeMainAfterSpawn(std::move(exe), std::move(a.req),
                         std::move(a.local_symbols));
    // Will never get here.
  }
  return absl::OkStatus();
}

void ZygoteCore::WaitForSpawn(co::Coroutine *c) {
  char *sendbuf = buffer_ + sizeof(int32_t);
  constexpr size_t kSendBufLen = sizeof(buffer_) - sizeof(int32_t);
  absl::StatusOr<ssize_t> n =
      control_socket_->ReceiveMessage(buffer_, sizeof(buffer_), c);
  if (!n.ok()) {
    logger_.Log(toolbelt::LogLevel::kError, "ReceiveMessage error %s\n",
                n.status().ToString().c_str());
    control_socket_->Close();
    return;
  }

  std::vector<toolbelt::FileDescriptor> fds;

  if (absl::Status status = control_socket_->ReceiveFds(fds, c); !status.ok()) {
    logger_.Log(toolbelt::LogLevel::kError, "Failed to receive fds %s\n",
                n.status().ToString().c_str());
    control_socket_->Close();
    return;
  }

  stagezero::control::SpawnRequest request;
  if (request.ParseFromArray(buffer_, *n)) {
    stagezero::control::SpawnResponse response;

    if (absl::Status status = HandleSpawn(request, &response, std::move(fds), c);
        !status.ok()) {
      response.set_error(status.ToString());
    }
    if (!response.SerializeToArray(sendbuf, kSendBufLen)) {
      logger_.Log(toolbelt::LogLevel::kError, "Failed to serialize response");
      return;
    }
    size_t msglen = response.ByteSizeLong();
    absl::StatusOr<ssize_t> n =
        control_socket_->SendMessage(sendbuf, msglen, c);
    if (!n.ok()) {
      logger_.Log(toolbelt::LogLevel::kError,
                  "Failed to send spawn response: %s",
                  n.status().ToString().c_str());
      control_socket_->Close();
      return;
    }
  } else {
    logger_.Log(toolbelt::LogLevel::kError,
                "Failed to parse message SpawnRequest");
    control_socket_->Close();
  }
}

void ZygoteCore::Run(const std::string &dso, const std::string &main,
                     const std::vector<std::string> &vars) {
  control::SpawnRequest spawn;
  spawn.set_dso(dso);
  spawn.set_main_func(main);
  SymbolTable symbols(&global_symbols_);
  for (auto &var : vars) {
    std::string name;
    std::string value;
    size_t equal = var.find('=');
    if (equal != std::string::npos) {
      name = var.substr(0, equal);
      value = var.substr(equal + 1);
      symbols.AddSymbol(name, value, false);
    } else {
      symbols.AddSymbol(name, "", false);
    }
  }
  InvokeMainAfterSpawn(std::move(args_[0]), std::move(spawn),
                       std::move(symbols));
}

absl::Status
ZygoteCore::HandleSpawn(const control::SpawnRequest &req,
                        control::SpawnResponse *resp,
                        std::vector<toolbelt::FileDescriptor> &&fds,
                        co::Coroutine* c) {
  // Update all global symbols.
  for (auto &var : req.global_vars()) {
    auto sym = global_symbols_.FindSymbol(var.name());
    if (sym == nullptr) {
      // New global symbol.
      global_symbols_.AddSymbol(var.name(), var.value(), var.exported());
    } else {
      // Existing symbol.
      sym->SetValue(var.value());
    }
  }

  SymbolTable local_symbols(&global_symbols_);
  for (auto &var : req.vars()) {
    local_symbols.AddSymbol(var.name(), var.value(), var.exported());
  }

  // It's better to look for the DSO now as reporting an error later
  // is after the fork and this isn't as good for the user.
  if (!req.dso().empty()) {
    struct stat st;
    std::string exe = local_symbols.ReplaceSymbols(req.dso());

    int e = ::stat(exe.c_str(), &st);
    if (e == -1) {
      // Can't find the shared object.
      return absl::InternalError(
          absl::StrFormat("Can't find shared object %s", exe));
    }
  } else {
    // No shared object to load.  Look for the main_func symbol in
    // the zygote.  Best to do it here so we can give a timely
    // error.
    if (req.main_func().empty()) {
      return absl::InternalError("No main_func specifed for virtual process");
    }
    std::string expanded_main = local_symbols.ReplaceSymbols(req.main_func());
    void *main_func = dlsym(nullptr, expanded_main.c_str());
    if (main_func == nullptr) {
      return absl::InternalError(
          absl::StrFormat("Failed to find main function %s", expanded_main));
    }
  }

  // All looks good for the attempt to fork the zygote.  Let's do it.
  pid_t pid = fork();
  if (pid == -1) {
    return absl::InternalError(
        absl::StrFormat("Failed to fork: %s", strerror(errno)));
  }
  if (pid == 0) {
    // Child.
    // Close control socket.
    control_socket_.reset();

    local_symbols.AddSymbol("pid", absl::StrFormat("%d", getpid()), false);

    // We want to close all file descriptors that the zygote has opened except
    // all the ones we want.
    absl::flat_hash_set<int> fds_to_keep_open;

    // Keep 0, 1 and 2 open.  If the stream config specifies that they
    // should be closed, they will be removed from the hash set.
    for (int i = 0; i < 3; i++) {
      fds_to_keep_open.insert(i);
    }

    // The notify fd is added as an exported symbol.
    if (req.has_notify_fd_index()) {
      toolbelt::FileDescriptor &notify_fd = fds[req.notify_fd_index()];
      fds_to_keep_open.insert(notify_fd.Fd());

      // The zygote's notify fd is in the current environment and we will
      // replace it.
      unsetenv("STAGEZERO_NOTIFY_FD");
      local_symbols.AddSymbol("STAGEZERO_NOTIFY_FD",
                              absl::StrFormat("%d", notify_fd.Fd()), true);
    }

    for (auto &stream : req.streams()) {
      if (stream.has_filename()) {
        // For files, we have deferred the open until we know the pid
        // of the process.  This is because it's likely that the
        // filename contains the PID of the process.
        int oflag = stream.direction() == proto::StreamControl::INPUT
                        ? O_RDONLY
                        : (O_WRONLY | O_TRUNC | O_CREAT);

        std::string filename = stream.filename();
        int file_fd =
            open(local_symbols.ReplaceSymbols(filename).c_str(), oflag, 0777);
        if (file_fd == -1) {
          std::cerr << "Failed to open file " << filename << ": "
                    << strerror(errno) << std::endl;
          exit(1);
        }

        fds_to_keep_open.insert(stream.fd());
        int e = dup2(file_fd, stream.fd());
        if (e == -1) {
          std::cerr << "Failed to redirect fd " << stream.fd() << ": "
                    << strerror(errno) << std::endl;
          exit(1);
        }
        continue;
      }
      (void)close(stream.fd());
      if (!stream.close()) {
        int e = dup2(fds[stream.index()].Fd(), stream.fd());
        if (e == -1) {
          std::cerr << "Failed to redirect fd " << stream.fd() << ": "
                    << strerror(errno) << std::endl;
          exit(1);
        }
        fds_to_keep_open.insert(stream.fd());
      } else {
        // Don't keep this open.
        fds_to_keep_open.erase(stream.fd());
      }
      fds[stream.index()].Reset();
    }

    // Close all open file descriptors that we don't want to explicitly
    // keep open.
    toolbelt::CloseAllFds(
        [&fds_to_keep_open](int fd) { return !fds_to_keep_open.contains(fd); });

    // We are in the child process running in the server coroutine context.
    // We want to invoke the virtual process's main function using the process's
    // main stack instead of the coroutine stack.  To do this, we stop the
    // coroutine scheduler in the child process and yield the server
    // coroutine.  This will switch to the scheduler's context which will
    // return from Run.
    after_fork.req = std::move(req);
    after_fork.local_symbols = std::move(local_symbols);
    forked_ = true;

    // Stop the coroutine scheduler in the child process.  This will cause
    // a return from its Run() function in ZygoteCore::Run, where we will
    // invoke the main function on the program stack.
    scheduler_.Stop();
    c->Yield();

    // Won't get here in the child process.
    return absl::OkStatus();
  }

  for (auto &stream : req.streams()) {
    if (stream.has_filename()) {
      // These are opened locally, not passed from streamzero.
      continue;
    }
    fds[stream.index()].Reset();
  }

  resp->set_pid(pid);
  return absl::OkStatus();
}

// This is called in the child process and does not return.
void ZygoteCore::InvokeMainAfterSpawn(std::string exe,
                                      const control::SpawnRequest &&req,
                                      SymbolTable &&local_symbols) {

  void *handle = RTLD_DEFAULT;
  if (!req.dso().empty()) {
    exe = local_symbols.ReplaceSymbols(req.dso());
    handle = dlopen(exe.c_str(), RTLD_LAZY);
    if (handle == nullptr) {
      std::cerr << "Failed to open DSO " << req.dso() << std::endl;
      exit(1);
    }
  }
  void *main_func =
      dlsym(handle, local_symbols.ReplaceSymbols(req.main_func()).c_str());
  if (main_func == nullptr) {
    std::cerr << "Failed to find main function " << req.main_func()
              << std::endl;
    exit(1);
  }

  // Build argv and env for main.

  std::vector<std::string> args;
  args.reserve(req.args().size());

  for (auto &arg : req.args()) {
    args.push_back(local_symbols.ReplaceSymbols(arg));
  }

  std::vector<const char *> argv;
  argv.push_back(exe.c_str()); // Executable.
  for (auto &arg : args) {
    argv.push_back(arg.c_str());
  }

  absl::flat_hash_map<std::string, Symbol *> env_vars =
      local_symbols.GetEnvironmentSymbols();
  for (auto & [ name, symbol ] : env_vars) {
    setenv(name.c_str(), symbol->Value().c_str(), 1);
  }

  signal(SIGPIPE, SIG_IGN);

  // Convert to a function pointer and call main.
  using Ptr =
      int (*)(SymbolTable && local_symbols, int, const char **, const char **);
  Ptr main = reinterpret_cast<Ptr>(main_func);
  exit(main(std::move(local_symbols), argv.size(), argv.data(), environ));
}

} // namespace stagezero
