// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/zygote/zygote_core.h"

#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "toolbelt/fd.h"
#include "toolbelt/hexdump.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include <string>

extern "C" const char **environ;

namespace stagezero {

ZygoteCore::ZygoteCore(int argc, char **argv) {
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
  }

  const char *sname = getenv("STAGEZERO_ZYGOTE_SOCKET_NAME");
  if (sname == nullptr) {
    return absl::InternalError(
        "No STAGEZERO_ZYGOTE_SOCKET_NAME passed to zygote");
  }

  co::CoroutineScheduler scheduler;
  std::string socket_name = sname;
  control_socket_ = std::make_unique<toolbelt::UnixSocket>();
  absl::Status status = control_socket_->Connect(socket_name);
  if (!status.ok()) {
    return absl::InternalError(absl::StrFormat(
        "Zygote failed to connect to control socket: %s", status.ToString()));
  }

  server_ =
      std::make_unique<co::Coroutine>(scheduler, [this](co::Coroutine *c) {
        while (control_socket_->Connected()) {
          WaitForSpawn(c);
        }

      });

  monitor_ =
      std::make_unique<co::Coroutine>(scheduler, [this](co::Coroutine *c) {
        constexpr int kWaitTimeMs = 500;

        for (;;) {
          int status;
          pid_t pid = waitpid(-1, &status, WNOHANG);
          if (pid > 0) {
            // A process died.
            logger_.Log(toolbelt::LogLevel::kInfo, "Process %d died", pid);
          }
          c->Millisleep(kWaitTimeMs);
        }

      });

  scheduler.Run();
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

    if (absl::Status status = HandleSpawn(request, &response, std::move(fds));
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

absl::Status ZygoteCore::HandleSpawn(
    const control::SpawnRequest &req, control::SpawnResponse *resp,
    std::vector<toolbelt::FileDescriptor> &&fds) {

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
      std::cerr << "Failed to find shared object " << exe << std::endl;
      return absl::InternalError(
          absl::StrFormat("Can't find shared object %s", exe));
    }
  }
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

    for (auto &stream : req.streams()) {
      (void)close(stream.fd());
      if (!stream.close()) {
        int e = dup2(fds[stream.index()].Fd(), stream.fd());
        if (e == -1) {
          std::cerr << "Failed to redirect fd " << stream.fd() << ": "
                    << strerror(errno) << std::endl;
          exit(1);
        }
      }
      fds[stream.index()].Reset();
    }
    InvokeMainAfterSpawn(std::move(req), std::move(local_symbols));
  }
  for (auto &stream : req.streams()) {
    fds[stream.index()].Reset();
  }
  resp->set_pid(pid);
  return absl::OkStatus();
}

// This is called in the child process and does not return.
void ZygoteCore::InvokeMainAfterSpawn(const control::SpawnRequest &&req,
                                      SymbolTable &&local_symbols) {
  void *handle = RTLD_DEFAULT;
  std::string exe = args_[0];
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
  argv.push_back(exe.c_str());  // Executable.
  for (auto &arg : args) {
    argv.push_back(arg.c_str());
  }

  absl::flat_hash_map<std::string, Symbol *> env_vars =
      local_symbols.GetEnvironmentSymbols();
  for (auto & [ name, symbol ] : env_vars) {
    setenv(name.c_str(), symbol->Value().c_str(), 1);
  }

  setpgrp();

  // Convert to a function pointer and call main.
  using Ptr = int (*)(int, const char **, const char **);
  Ptr main = reinterpret_cast<Ptr>(main_func);
  exit(main(argv.size(), argv.data(), environ));
}

}  // namespace stagezero
