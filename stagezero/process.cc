// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/process.h"
#include "stagezero/stagezero.h"

#include "absl/strings/str_format.h"
#include "common/stream.h"
#include "stagezero/cgroup.h"
#include "stagezero/client_handler.h"
#include "toolbelt/hexdump.h"

#include <ctype.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <limits.h>
#include <sys/wait.h>
#include <vector>
#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#else
#error "Unknown OS"
#endif
#include <grp.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <termios.h>

namespace adastra::stagezero {

Process::Process(co::CoroutineScheduler &scheduler,
                 std::shared_ptr<ClientHandler> client, std::string name)
    : scheduler_(scheduler), client_(std::move(client)), name_(std::move(name)),
      local_symbols_(client_->GetGlobalSymbols()) {
  // Add a locall symbol "name" for the process name.
  local_symbols_.AddSymbol("name", name_, false);
}

void Process::SetProcessId() {
  process_id_ = absl::StrFormat("%s/%s@%s:%d", client_->GetClientName(), name_,
                                client_->GetCompute(), pid_);
}

void Process::KillNow() {
  client_->Log(Name(), toolbelt::LogLevel::kDebug,
               "Killing process %s with pid %d", Name().c_str(), pid_);
  if (pid_ <= 0) {
    return;
  }
  SafeKill(pid_, SIGKILL);
  (void)client_->RemoveProcess(this);
}

int Process::WaitLoop(co::Coroutine *c,
                      std::optional<std::chrono::seconds> timeout) {
  constexpr int kWaitTimeMs = 100;
  int num_iterations =
      timeout.has_value() ? timeout->count() * 1000 / kWaitTimeMs : 0;
  int status = 0;
  for (;;) {
    status = Wait();
    if (!running_) {
      break;
    }
    c->Millisleep(kWaitTimeMs);
    if (timeout.has_value()) {
      num_iterations--;
      if (num_iterations == 0) {
        break;
      }
    }
  }
  return status;
}

const std::shared_ptr<StreamInfo> Process::FindNotifyStream() const {
  for (auto &stream : streams_) {
    if (stream->disposition == proto::StreamControl::NOTIFY) {
      return stream;
    }
  }
  return nullptr;
}

StaticProcess::StaticProcess(
    co::CoroutineScheduler &scheduler, std::shared_ptr<ClientHandler> client,
    const stagezero::control::LaunchStaticProcessRequest &&req)
    : Process(scheduler, std::move(client), req.opts().name()),
      req_(std::move(req)) {
  for (auto &var : req.opts().vars()) {
    local_symbols_.AddSymbol(var.name(), var.value(), var.exported());
  }
  SetSignalTimeouts(req.opts().sigint_shutdown_timeout_secs(),
                    req.opts().sigterm_shutdown_timeout_secs());
  SetUserAndGroup(req.opts().user(), req.opts().group());
  SetCgroup(req.opts().cgroup());
  interactive_ = req.opts().interactive();
  if (req.opts().has_interactive_terminal()) {
    interactive_terminal_.FromProto(req.opts().interactive_terminal());
  }
  critical_ = req.opts().critical();
}

absl::Status StaticProcess::Start(co::Coroutine *c) {
  return StartInternal({}, true);
}

absl::Status
StaticProcess::StartInternal(const std::vector<std::string> extra_env_vars,
                             bool send_start_event) {
  if (absl::Status status = ValidateStreams(req_.streams()); !status.ok()) {
    return status;
  }
  if (absl::Status status = ForkAndExec(extra_env_vars); !status.ok()) {
    return status;
  }
  // Generate process id.
  SetProcessId();

  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [ proc = shared_from_this(), client = client_,
        send_start_event ](co::Coroutine * c) {
        if (proc->WillNotify()) {
          std::shared_ptr<StreamInfo> s = proc->FindNotifyStream();
          if (s != nullptr) {
            int notify_fd = s->pipe.ReadFd().Fd();
            uint64_t timeout_ns = proc->StartupTimeoutSecs() * 1000000000LL;
            int wait_fd = c->Wait(notify_fd, POLLIN, timeout_ns);
            if (wait_fd == -1) {
              // Timeout waiting for notification.
              client->Log(
                  proc->Name(), toolbelt::LogLevel::kError,
                  "Process %s failed to notify us of startup after %d seconds",
                  proc->Name().c_str(), proc->StartupTimeoutSecs());

              // Stop the process as it failed to notify us.
              (void)proc->Stop(c);
              return;
            }
            // Read the data from the notify pipe.
            int64_t val;
            (void)read(notify_fd, &val, 8);
            // Nothing to interpret from this (yet?)

            client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                        "Process %s notified us of startup",
                        proc->Name().c_str());
          }
        }
        // Send start event to client.
        if (send_start_event) {
          absl::Status eventStatus =
              client->SendProcessStartEvent(proc->GetId());
          if (!eventStatus.ok()) {
            client->Log(proc->Name(), toolbelt::LogLevel::kError, "%s\n",
                        eventStatus.ToString().c_str());
            return;
          }
        }
        int status = proc->WaitLoop(c, std::nullopt);
        // The process might have died due to an external signal.  If we didn't
        // kill it, we won't have removed it from the maps.  We try to do this
        // now but ignore it if it's already gone.
        client->TryRemoveProcess(proc);

        if (absl::Status status = proc->RemoveFromCgroup(proc->GetPid()); !status.ok()) {
          client->Log(proc->Name(), toolbelt::LogLevel::kError, "%s\n",
                      status.ToString().c_str());
          return;
        }

        bool signaled = WIFSIGNALED(status);
        bool exited = WIFEXITED(status);
        int term_sig = WTERMSIG(status);
        int exit_status = WEXITSTATUS(status);
        // Can't be both exit and signal, but can be neither in the case
        // of a stop.  We don't expect anything to be stopped and don't
        // support it.
        if (!signaled && !exited) {
          signaled = true;
        }
        if (exited) {
          client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                      "Static process %s exited with status %d",
                      proc->Name().c_str(), exit_status);
        } else {
          client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                      "Static process %s received signal %d \"%s\"",
                      proc->Name().c_str(), term_sig, strsignal(term_sig));
        }
        if (absl::Status eventStatus = client->SendProcessStopEvent(
                proc->GetId(), !signaled, exit_status, term_sig);
            !eventStatus.ok()) {
          client->Log(proc->Name(), toolbelt::LogLevel::kError, "%s\n",
                      eventStatus.ToString().c_str());
          return;
        }
      },
      name_.c_str()));
  return absl::OkStatus();
}

// Returns a pair of open file descriptors.  The first is the stagezero end
// and the second is the process end.
static absl::StatusOr<toolbelt::Pipe>
MakeFileDescriptors(bool istty, const proto::Terminal *term) {
  if (istty) {
    int this_end, proc_end;

    struct winsize win = {};
    ioctl(0, TIOCGWINSZ, &win); // Might fail.

    if (term != nullptr) {
      win.ws_col = term->cols();
      win.ws_row = term->rows();
    }
    if (win.ws_col == 0) {
      win.ws_col = 80;
    }
    if (win.ws_row == 0) {
      win.ws_row = 24;
    }

    int e = openpty(&this_end, &proc_end, nullptr, nullptr, &win);
    if (e == -1) {
      return absl::InternalError(absl::StrFormat(
          "Failed to open pty for stream: %s", strerror(errno)));
    }

    absl::StatusOr<toolbelt::Pipe> p =
        toolbelt::Pipe::Create(this_end, proc_end);
    if (!p.ok()) {
      return p.status();
    }
    return p;
  }
  absl::StatusOr<toolbelt::Pipe> p = toolbelt::Pipe::Create();
  if (!p.ok()) {
    return p.status();
  }
  return p;
}

absl::Status StreamFromFileDescriptor(
    int fd, std::function<absl::Status(const char *, size_t)> writer,
    co::Coroutine *c) {
  char buffer[256];
  for (;;) {
    int wait_fd = c->Wait(fd, POLLIN);
    if (wait_fd != fd) {
      return absl::InternalError("Interrupted");
    }
    ssize_t n = ::read(fd, buffer, sizeof(buffer));
    if (n <= 0) {
      if (n == -1) {
        return absl::InternalError(
            absl::StrFormat("stream failed: %s", strerror(errno)));
      }
      return absl::OkStatus();
    }
    if (absl::Status status = writer(buffer, n); !status.ok()) {
      return status;
    }
  }
}

absl::Status WriteToProcess(int fd, const char *buf, size_t len,
                            co::Coroutine *c) {
  size_t remaining = len;
  while (remaining > 0) {
    int wait_fd = c->Wait(fd, POLLOUT);
    if (wait_fd != fd) {
      return absl::InternalError("Interrupted");
    }
    ssize_t n = ::write(fd, buf, remaining);
    if (n <= 0) {
      if (n == -1) {
        return absl::InternalError(
            absl::StrFormat("stream failed: %s", strerror(errno)));
      }
      break;
    }
    remaining -= n;
    buf += n;
  }
  return absl::OkStatus();
}

static void DefaultStreamDirections(std::shared_ptr<StreamInfo> stream) {
  if (stream->disposition == proto::StreamControl::CLOSE ||
      stream->disposition == proto::StreamControl::STAGEZERO) {
    return;
  }

  switch (stream->fd) {
  case STDIN_FILENO:
    if (stream->direction != proto::StreamControl::DEFAULT) {
      stream->direction = proto::StreamControl::INPUT;
    }
    break;
  case STDOUT_FILENO:
  case STDERR_FILENO:
    if (stream->direction != proto::StreamControl::DEFAULT) {
      stream->direction = proto::StreamControl::OUTPUT;
    }
    break;
  }
}

absl::Status Process::BuildStreams(
    const google::protobuf::RepeatedPtrField<proto::StreamControl> &streams,
    bool notify) {
  // If the process is going to notify us of startup, make a pipe
  // for it to use and build a StreamInfo for it.
  if (notify) {
    auto stream = std::make_shared<StreamInfo>();
    stream->disposition = proto::StreamControl::NOTIFY;
    stream->direction = proto::StreamControl::OUTPUT;
    absl::StatusOr<toolbelt::Pipe> pipe = MakeFileDescriptors(false, nullptr);
    if (!pipe.ok()) {
      return pipe.status();
    }
    stream->fd = pipe->WriteFd().Fd();
    stream->pipe = std::move(*pipe);
    streams_.push_back(stream);
  }

  if (interactive_) {
    int this_end, proc_end;

    struct winsize win = {};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &win); // Might fail.

    if (interactive_terminal_.IsPresent()) {
      win.ws_col = interactive_terminal_.cols;
      win.ws_row = interactive_terminal_.rows;
    }
    if (win.ws_col == 0) {
      win.ws_col = 80;
    }
    if (win.ws_row == 0) {
      win.ws_row = 24;
    }
    int e = openpty(&this_end, &proc_end, nullptr, nullptr, &win);
    if (e == -1) {
      return absl::InternalError(absl::StrFormat(
          "Failed to open pty for interactive stream: %s", strerror(errno)));
    }
    interactive_this_end_.SetFd(this_end);
    interactive_proc_end_.SetFd(proc_end);

    // Spawn coroutine to read from the pty and send output events.
    client_->AddCoroutine(std::make_unique<co::Coroutine>(
        scheduler_, [ proc = shared_from_this(), client = client_,
                      this_end ](co::Coroutine * c) {
          absl::Status status = StreamFromFileDescriptor(
              this_end,
              [proc, client](const char *buf, size_t len) -> absl::Status {
                // Write to client using an event.
                return client->SendOutputEvent(proc->GetId(), STDOUT_FILENO,
                                               buf, len);
              },
              c);
        }));
  }

  for (const proto::StreamControl &s : streams) {
    auto stream = std::make_shared<StreamInfo>();
    proto::StreamControl::Direction direction = s.direction();
    stream->direction = direction;
    stream->fd = s.stream_fd();
    stream->disposition = s.disposition();
    stream->tty = s.tty();
    DefaultStreamDirections(stream);
    streams_.push_back(stream);

    switch (stream->disposition) {
    case proto::StreamControl::CLOSE:
    case proto::StreamControl::STAGEZERO:
      break;
    case proto::StreamControl::CLIENT: {
      absl::StatusOr<toolbelt::Pipe> pipe = MakeFileDescriptors(
          s.tty(), s.has_terminal() ? &s.terminal() : nullptr);
      if (!pipe.ok()) {
        return pipe.status();
      }
      stream->pipe = std::move(*pipe);

      if (s.has_terminal()) {
        stream->term_name = s.terminal().name();
      }
      // For an output stream start a coroutine to read from the pipe/tty
      // and send as an event.
      //
      // An input stream is handled by an incoming InputData command that
      // writes to the write end of the pipe/tty.
      if (stream->direction == proto::StreamControl::OUTPUT) {
        client_->AddCoroutine(std::make_unique<co::Coroutine>(
            scheduler_, [ proc = shared_from_this(), stream,
                          client = client_ ](co::Coroutine * c) {
              absl::Status status = StreamFromFileDescriptor(
                  stream->pipe.ReadFd().Fd(),
                  [proc, stream, client](const char *buf,
                                         size_t len) -> absl::Status {
                    // Write to client using an event.
                    return client->SendOutputEvent(proc->GetId(), stream->fd,
                                                   buf, len);
                  },
                  c);
            }));
      }
      break;
    }

    case proto::StreamControl::LOGGER: {
      absl::StatusOr<toolbelt::Pipe> pipe = MakeFileDescriptors(
          s.tty(), s.has_terminal() ? &s.terminal() : nullptr);
      if (!pipe.ok()) {
        return pipe.status();
      }
      stream->pipe = std::move(*pipe);

      if (s.has_terminal()) {
        stream->term_name = s.terminal().name();
      }
      client_->AddCoroutine(std::make_unique<co::Coroutine>(
          scheduler_, [ proc = shared_from_this(), stream,
                        client = client_ ](co::Coroutine * c) {
            absl::Status status = StreamFromFileDescriptor(
                stream->pipe.ReadFd().Fd(),
                [proc, stream, client](const char *buf,
                                       size_t len) -> absl::Status {
                  // Write a log message to the client using an event.
                  return client->SendLogMessage(
                      stream->fd == 1 ? toolbelt::LogLevel::kInfo
                                      : toolbelt::LogLevel::kError,
                      proc->Name(), std::string(buf, len));
                },
                c);
          }));

      break;
    }
    case proto::StreamControl::FILENAME: {
      std::string filename = s.filename();
      if (filename.empty()) {
        // An empty filename means we use a good default.
        filename = absl::StrFormat("$logdir/$name.%d.$pid.log", stream->fd);
      }
      stream->filename = filename;
      // Defer the opening of the filename until we know the PID of the process.
      break;
    }
    case proto::StreamControl::FD:
      // Set the process end of the stream (the fd that will be redirected)
      // to the fd specified by the user.
      if (stream->direction == proto::StreamControl::OUTPUT) {
        stream->pipe.SetWriteFd(s.fd());
      } else {
        stream->pipe.SetReadFd(s.fd());
      }
      break;
    default:
      break;
    }
  }
  return absl::OkStatus();
}

absl::Status
StaticProcess::ForkAndExec(const std::vector<std::string> extra_env_vars) {
  // Set up streams.
  if (absl::Status status = BuildStreams(req_.streams(), req_.opts().notify());
      !status.ok()) {
    return status;
  }
  uid_t uid = geteuid();
  gid_t gid = getegid();

  if (!user_.empty()) {
    struct passwd *p = getpwnam(user_.c_str());
    if (p == nullptr) {
      return absl::InternalError(absl::StrFormat("Unknown user %s", user_));
    }
    uid = p->pw_uid;
    gid = p->pw_gid;
  }

  if (!group_.empty()) {
    struct group *g = getgrnam(group_.c_str());
    if (g == nullptr) {
      return absl::InternalError(absl::StrFormat("Unknown group %s", group_));
    }
    gid = g->gr_gid;
  }

  pid_ = fork();
  if (pid_ == -1) {
    return absl::InternalError(
        absl::StrFormat("Fork failed: %s", strerror(errno)));
  }
  if (pid_ == 0) {
    // Set some local variables for the process.
    local_symbols_.AddSymbol("pid", absl::StrFormat("%d", getpid()), false);

    // Stop the coroutine scheduler in this process.  We have forked so the
    // all the coroutines in the parent process are also in this child process.
    // We don't want them to run in the child, so we stop the scheduler.
    // We will be calling exec so all the memory in the child process will
    // be freed.
    client_->StopAllCoroutines();
    client_->GetScheduler().Stop();

    // Redirect the streams.
    if (interactive_) {
      setsid();
      int e = ioctl(interactive_proc_end_.Fd(), TIOCSCTTY, 0);
      if (e != 0) {
        std::cerr << "unable to make controlling terminal: " << strerror(errno)
                  << "\n";
      }
      interactive_this_end_.Reset();
      for (int i = 0; i < 3; i++) {
        ::close(i);
        int e = dup2(interactive_proc_end_.Fd(), i);
        if (e == -1) {
          std::cerr << "Failed to redirect interactive file descriptor: " << i
                    << " " << strerror(errno) << std::endl;
          exit(1);
        }
      }
      interactive_proc_end_.Reset();
    }

    for (auto &stream : streams_) {
      if (stream->disposition != proto::StreamControl::CLOSE &&
          stream->disposition != proto::StreamControl::STAGEZERO) {
        // For a notify we don't redirect an fd, but instead tell
        // the process what it is via an environment variable.
        // For other streams, we redirect to the given file descriptor
        // number and close the duplicated file descriptor.
        if (stream->disposition != proto::StreamControl::NOTIFY) {
          if (stream->disposition == proto::StreamControl::FILENAME) {
            // For files, we have deferred the open until we know the pid
            // of the process.  This is because it's likely that the
            // filename contains the PID of the process.
            int oflag = stream->direction == proto::StreamControl::INPUT
                            ? O_RDONLY
                            : (O_WRONLY | O_TRUNC | O_CREAT);

            std::string filename = stream->filename;
            int file_fd = open(local_symbols_.ReplaceSymbols(filename).c_str(),
                               oflag, 0777);
            if (file_fd == -1) {
              std::cerr << "Failed to open file " << filename << ": "
                        << strerror(errno) << std::endl;
              exit(1);
            }
            // Set the process end of the stream (the fd that will be
            // redirected) to the file's open fd.
            if (stream->direction == proto::StreamControl::OUTPUT) {
              stream->pipe.SetWriteFd(file_fd);
            } else {
              stream->pipe.SetReadFd(file_fd);
            }
          }
          toolbelt::FileDescriptor &fd =
              stream->direction == proto::StreamControl::OUTPUT
                  ? stream->pipe.WriteFd()
                  : stream->pipe.ReadFd();
          (void)close(stream->fd);

          int e = dup2(fd.Fd(), stream->fd);
          if (e == -1) {
            std::cerr << "Failed to redirect file descriptor: "
                      << strerror(errno) << std::endl;
            exit(1);
          }

          if (!stream->term_name.empty()) {
            // Set the TERM environment variable to the terminal name given.
            local_symbols_.AddSymbol("TERM", stream->term_name, true);
          }

          // Close the duplicated fd.
          fd.Reset();
        }

        if (stream->disposition == proto::StreamControl::CLIENT ||
            stream->disposition == proto::StreamControl::NOTIFY) {
          // Close the duplicated other end of the pipes.
          toolbelt::FileDescriptor &fd =
              stream->direction == proto::StreamControl::OUTPUT
                  ? stream->pipe.ReadFd()
                  : stream->pipe.WriteFd();
          fd.Reset();
        }
      }
    }

    std::shared_ptr<StreamInfo> notify_stream = FindNotifyStream();
    if (req_.opts().notify() && notify_stream != nullptr) {
      // Add a local symbol for the notify stream.  This can be see by the
      // arguments.
      local_symbols_.AddSymbol(
          "notify_fd",
          absl::StrFormat("%d", notify_stream->pipe.WriteFd().Fd()), false);
    }

    // Copy args with symbols replaced into local memory to keep the
    // strings around for the argv array.
    std::vector<std::string> args;
    args.reserve(req_.opts().args_size());
    for (auto &arg : req_.opts().args()) {
      args.push_back(local_symbols_.ReplaceSymbols(arg));
    }

    // Build the argv array for execve.
    std::vector<const char *> argv;
    std::string exe = local_symbols_.ReplaceSymbols(req_.proc().executable());
    client_->Log(Name(), toolbelt::LogLevel::kInfo, "Starting %s", exe.c_str());
    argv.push_back(exe.c_str());
    for (auto &arg : args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    // Build the environment strings.
    std::vector<std::string> env_strings;
    if (req_.opts().notify() && notify_stream != nullptr) {
      // For a notify fd, set the STAGEZERO_NOTIFY_FD environment
      // variable.  The process will write a 8 arbitrary bytes to this
      // to tell us that it has started.
      env_strings.push_back(absl::StrFormat(
          "STAGEZERO_NOTIFY_FD=%d", notify_stream->pipe.WriteFd().Fd()));
    }
    absl::flat_hash_map<std::string, Symbol *> env_vars =
        local_symbols_.GetEnvironmentSymbols();
    for (auto & [ name, symbol ] : env_vars) {
      env_strings.push_back(absl::StrFormat("%s=%s", name, symbol->Value()));
    }
    for (auto &extra : extra_env_vars) {
      env_strings.push_back(extra);
    }

    std::vector<const char *> env;
    env.reserve(env_strings.size());
    for (auto &var : env_strings) {
      env.push_back(var.c_str());
    }
    env.push_back(nullptr);

    setpgrp();

    if (geteuid() == 0) {
      // We can only set the user and group if we are running as root.
      seteuid(uid);
      setegid(gid);
    }

    // Add the process to the cgroup if it is set.
    if (absl::Status status = AddToCgroup(getpid()); !status.ok()) {
      std::cerr << "Failed to add process to cgroup " << cgroup_ << status
                << std::endl;
      exit(1);
    }

    execve(exe.c_str(),
           reinterpret_cast<char *const *>(const_cast<char **>(argv.data())),
           reinterpret_cast<char *const *>(const_cast<char **>(env.data())));
    std::cerr << "Failed to exec " << argv[0] << ": " << strerror(errno)
              << std::endl;
    exit(1);
  }

  // Close redirected stream fds in parent.
  if (interactive_) {
    // The proc end has been duplicated in the child and we don't need it
    // in the parent.
    interactive_proc_end_.Reset();
    // This end is what we read and write.
  }
  for (auto &stream : streams_) {
    if (stream->disposition != proto::StreamControl::CLOSE &&
        stream->disposition != proto::StreamControl::STAGEZERO &&
        stream->disposition != proto::StreamControl::NOTIFY) {
      toolbelt::FileDescriptor &fd =
          stream->direction == proto::StreamControl::OUTPUT
              ? stream->pipe.WriteFd()
              : stream->pipe.ReadFd();
      fd.Reset();
    }
  }
  return absl::OkStatus();
}

int StaticProcess::Wait() {
  int status = 0;
  pid_t pid = waitpid(pid_, &status, WNOHANG);
  if (pid == 0) {
    // Process is running.
    return 0;
  }
  if (pid == pid_) {
    // Process has exited.
    running_ = false;
  }
  return status;
}

absl::Status Process::Stop(co::Coroutine *c) {
  stopping_ = true;
  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [ proc = shared_from_this(), client = client_ ](co::Coroutine * c2) {
        if (!proc->IsRunning()) {
          return;
        }
        int timeout = proc->SigIntTimeoutSecs();
        if (timeout > 0) {
          client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                      "Killing process %s with SIGINT (timeout %d seconds)",
                      proc->Name().c_str(), timeout);
          SafeKill(proc->GetPid(), SIGINT);
          (void)proc->WaitLoop(c2, std::chrono::seconds(timeout));
          if (!proc->IsRunning()) {
            return;
          }
        }
        timeout = proc->SigTermTimeoutSecs();
        if (timeout > 0) {
          SafeKill(proc->GetPid(), SIGTERM);
          client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                      "Killing process %s with SIGTERM (timeout %d seconds)",
                      proc->Name().c_str(), timeout);
          (void)proc->WaitLoop(c2, std::chrono::seconds(timeout));
        }

        // Always send SIGKILL if it's still running.  It can't ignore this.
        if (proc->IsRunning()) {
          client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                      "Killing process %s with SIGKILL", proc->Name().c_str());

          SafeKill(proc->GetPid(), SIGKILL);
        }
      }));
  return client_->RemoveProcess(this);
}

absl::Status Process::SendInput(int fd, const std::string &data,
                                co::Coroutine *c) {
  if (interactive_) {
    return WriteToProcess(interactive_this_end_.Fd(), data.data(), data.size(),
                          c);
  }
  for (auto &stream : streams_) {
    if (stream->fd == fd &&
        stream->disposition == proto::StreamControl::CLIENT &&
        stream->direction == proto::StreamControl::INPUT) {
      return WriteToProcess(stream->pipe.WriteFd().Fd(), data.data(),
                            data.size(), c);
    }
  }
  return absl::InternalError(absl::StrFormat("Unknown stream fd %d", fd));
}

absl::Status Process::CloseFileDescriptor(int fd) {
  for (auto &stream : streams_) {
    if (stream->fd == fd &&
        stream->disposition == proto::StreamControl::CLIENT &&
        stream->direction == proto::StreamControl::INPUT) {
      int stream_fd = stream->direction == proto::StreamControl::INPUT
                          ? stream->pipe.WriteFd().Fd()
                          : stream->pipe.ReadFd().Fd();
      int e = close(stream_fd);
      if (e == -1) {
        return absl::InternalError(
            absl::StrFormat("Failed to close fd %d: %s", fd, strerror(errno)));
      }
      return absl::OkStatus();
    }
  }
  return absl::InternalError(absl::StrFormat("Unknown stream fd %d", fd));
}

absl::Status Zygote::Start(co::Coroutine *c) {
  // Open a listening Unix Domain Socket for the zygote to connect to.
  toolbelt::UnixSocket listen_socket;

  std::pair<std::string, int> socket_and_fd = BuildZygoteSocketName();
  std::string socket_name = socket_and_fd.first;
  remove(socket_name.c_str());
  absl::Status status = listen_socket.Bind(socket_name, true);

  // Close temp file after we've opened the socket.
  close(socket_and_fd.second);
  if (!status.ok()) {
    return status;
  }

  std::vector<std::string> zygoteEnv = {
      absl::StrFormat("STAGEZERO_ZYGOTE_SOCKET_NAME=%s", socket_name)};

  if (absl::Status status =
          StartInternal(zygoteEnv, /*send_start_event=*/false);
      !status.ok()) {
    return status;
  }

  // Wait for the zygote to connect to the socket in a coroutine.
  auto acceptor = new co::Coroutine(scheduler_, [
    proc = shared_from_this(), listen_socket = std::move(listen_socket),
    client = client_
  ](co::Coroutine * c) mutable {
    absl::StatusOr<toolbelt::UnixSocket> s = listen_socket.Accept(c);
    if (!s.ok()) {
      client->Log(proc->Name(), toolbelt::LogLevel::kError,
                  "Unable to accept connection from zygote %s: %s",
                  proc->Name().c_str(), s.status().ToString().c_str());
      return;
    }
    auto zygote = std::static_pointer_cast<Zygote>(proc);
    zygote->SetControlSocket(std::move(*s));
    client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                "Zygote control socket open");
    absl::Status eventStatus = client->SendProcessStartEvent(proc->GetId());
    if (!eventStatus.ok()) {
      client->Log(proc->Name(), toolbelt::LogLevel::kError, "%s\n",
                  eventStatus.ToString().c_str());
      return;
    }
    // Read from notification pipe to detect process death notifications.
    std::shared_ptr<StreamInfo> notify_stream = proc->FindNotifyStream();
    if (notify_stream != nullptr) {
      int notify_fd = notify_stream->pipe.ReadFd().Fd();

      // Close write end now that zygote has it open.
      notify_stream->pipe.WriteFd().Reset();

      for (;;) {
        uint64_t pid_and_status;
        c->Wait(notify_fd);

        ssize_t n = read(notify_fd, &pid_and_status, sizeof(pid_and_status));
        if (n <= 0) {
          // EOF or error means zygote is no longer running.  Close all the
          // notification pipes to the virtual processes using the zygote.
          zygote->ForeachVirtualProcess(
              [](std::shared_ptr<VirtualProcess> p) { p->CloseNotifyPipe(); });
          return;
        }
        if (n == sizeof(pid_and_status) &&
            (pid_and_status & (1LL << 63)) != 0) {
          // Zygote notified us of a process going down.  This contains
          // both the process id and the status.
          int pid = (pid_and_status >> 32) & 0x7fffffff;
          int64_t status = pid_and_status & 0xffffffff;
          StageZero &stagezero = client->GetStageZero();
          auto p = stagezero.FindVirtualProcess(pid);
          if (p != nullptr) {
            p->Notify(status);
          } else {
            client->Log(proc->Name(), toolbelt::LogLevel::kError,
                        "Can't find virtual process %d\n", pid);
          }
        }
      }
    }

  });
  client_->AddCoroutine(std::unique_ptr<co::Coroutine>(acceptor));
  return absl::OkStatus();
}

std::pair<std::string, int> Zygote::BuildZygoteSocketName() {
  char socket_file[NAME_MAX]; // Unique file in file system.
  snprintf(socket_file, sizeof(socket_file), "/tmp/zygote-%s.XXXXXX",
           Name().c_str());
  int tmpfd = mkstemp(socket_file);
  return std::make_pair(socket_file, tmpfd);
}

absl::StatusOr<int>
Zygote::Spawn(const stagezero::control::LaunchVirtualProcessRequest &req,
              const std::vector<std::shared_ptr<StreamInfo>> &streams) {
  control::SpawnRequest spawn;
  spawn.set_name(req.opts().name());
  spawn.set_dso(req.proc().dso());
  spawn.set_main_func(req.proc().main_func());

  // Streams.
  std::vector<toolbelt::FileDescriptor> fds;
  int fd_index = 0;
  for (auto &stream : streams) {
    switch (stream->disposition) {
    case proto::StreamControl::CLOSE: {
      auto *s = spawn.add_streams();
      s->set_fd(stream->fd);
      s->set_close(true);
      break;
    }
    case proto::StreamControl::FILENAME: {
      auto *s = spawn.add_streams();
      s->set_fd(stream->fd);
      s->set_filename(stream->filename);
      s->set_direction(stream->direction);
      // The file is opened by spawned process since it knows the PID.
      break;
    }
    case proto::StreamControl::NOTIFY:
      fds.push_back(stream->pipe.WriteFd());
      spawn.set_notify_fd_index(fd_index++);
      break;
    default:
      if (stream->disposition != proto::StreamControl::STAGEZERO) {
        const toolbelt::FileDescriptor &fd =
            stream->direction == proto::StreamControl::OUTPUT
                ? stream->pipe.WriteFd()
                : stream->pipe.ReadFd();
        auto *s = spawn.add_streams();
        s->set_fd(stream->fd);
        fds.push_back(fd);
        s->set_index(fd_index++);
        break;
      }
    }
  }

  // Global varables
  auto globals = client_->GetGlobalSymbols();
  for (auto &g : globals->GetSymbols()) {
    auto *v = spawn.add_vars();
    v->set_name(g.first);
    v->set_value(g.second->Value());
    v->set_exported(g.second->Exported());
  }

  // Add process name as "name"
  auto *name = spawn.add_vars();
  name->set_name("name");
  name->set_value(req.opts().name());
  name->set_exported(false);

  // Local Variables.
  for (auto &var : req.opts().vars()) {
    auto *v = spawn.add_vars();
    *v = var;
  }

  // Args.
  for (auto &arg : req.opts().args()) {
    auto *a = spawn.add_args();
    *a = arg;
  }

  spawn.set_user(req.opts().user());
  spawn.set_group(req.opts().group());
  spawn.set_cgroup(req.opts().cgroup());

  std::vector<char> buffer(spawn.ByteSizeLong() + sizeof(int32_t));
  char *buf = buffer.data() + sizeof(int32_t);
  size_t buflen = buffer.size() - sizeof(int32_t);
  if (!spawn.SerializeToArray(buf, buflen)) {
    return absl::InternalError("Failed to serilize spawn message");
  }

  absl::StatusOr<ssize_t> n = control_socket_.SendMessage(buf, buflen);
  if (!n.ok()) {
    control_socket_.Close();
    return n.status();
  }

  if (absl::Status s = control_socket_.SendFds(fds); !s.ok()) {
    control_socket_.Close();
    return s;
  }

  // Wait for response and put it in the same buffer we used for send.
  n = control_socket_.ReceiveMessage(buffer.data(), buffer.size());
  if (!n.ok()) {
    control_socket_.Close();
    return n.status();
  }

  control::SpawnResponse response;
  if (!response.ParseFromArray(buffer.data(), *n)) {
    control_socket_.Close();
    return absl::InternalError("Failed to parse response");
  }
  if (!response.error().empty()) {
    return absl::InternalError(response.error());
  }
  return response.pid();
}

VirtualProcess::VirtualProcess(
    co::CoroutineScheduler &scheduler, std::shared_ptr<ClientHandler> client,
    const stagezero::control::LaunchVirtualProcessRequest &&req)
    : Process(scheduler, std::move(client), req.opts().name()),
      req_(std::move(req)) {
  for (auto &var : req.opts().vars()) {
    local_symbols_.AddSymbol(var.name(), var.value(), var.exported());
  }
  SetSignalTimeouts(req.opts().sigint_shutdown_timeout_secs(),
                    req.opts().sigterm_shutdown_timeout_secs());
  SetUserAndGroup(req.opts().user(), req.opts().group());
  SetCgroup(req.opts().cgroup());
  critical_ = req.opts().critical();

  // Create the notification pipe for zygote notifications.
  absl::StatusOr<toolbelt::Pipe> pipe = toolbelt::Pipe::Create();
  if (!pipe.ok()) {
    abort();
  }
  notify_pipe_ = *pipe;
}

absl::Status VirtualProcess::Start(co::Coroutine *c) {
  auto proc = shared_from_this();
  auto vproc = std::static_pointer_cast<VirtualProcess>(proc);

  zygote_ = client_->FindZygote(req_.proc().zygote());
  if (zygote_ == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No such zygote %s", req_.proc().zygote()));
  }

  if (absl::Status status = ValidateStreams(req_.streams()); !status.ok()) {
    return status;
  }
  if (absl::Status status = BuildStreams(req_.streams(), req_.opts().notify());
      !status.ok()) {
    return status;
  }

  absl::StatusOr<int> pid = zygote_->Spawn(req_, GetStreams());
  if (!pid.ok()) {
    return absl::InternalError(
        absl::StrFormat("Failed to spawn virtual process %s: %s", Name(),
                        pid.status().ToString()));
  }
  SetPid(*pid);
  SetProcessId();

  zygote_->AddVirtualProcess(vproc);
  if (!client_->GetStageZero().AddVirtualProcess(*pid, vproc)) {
    return absl::InternalError(
        absl::StrFormat("Failed to add virtual process %s", Name()));
  }

  if (req_.opts().notify()) {
    // Wait for notification from process.
    std::shared_ptr<StreamInfo> s = FindNotifyStream();
    if (s != nullptr) {
      int notify_fd = s->pipe.ReadFd().Fd();
      uint64_t timeout_ns = StartupTimeoutSecs() * 1000000000LL;
      int wait_fd = c->Wait(notify_fd, POLLIN, timeout_ns);
      if (wait_fd == -1) {
        // Timeout waiting for notification.
        client_->Log(
            Name(), toolbelt::LogLevel::kError,
            "Process %s failed to notify us of startup after %d seconds",
            Name().c_str(), StartupTimeoutSecs());

        // Stop the process as it failed to notify us.
        (void)Stop(c);
        return absl::OkStatus();
      }
      // Read the data from the notify pipe.
      int64_t val;
      (void)read(notify_fd, &val, 8);
      // Nothing to interpret from this (yet?)

      client_->Log(Name(), toolbelt::LogLevel::kDebug,
                   "Process %s notified us of startup", Name().c_str());
    }
  }

  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [ proc, vproc, zygote = zygote_, client = client_ ](co::Coroutine * c2) {
        // Send start event to client.
        absl::Status eventStatus = client->SendProcessStartEvent(proc->GetId());
        if (!eventStatus.ok()) {
          client->Log(proc->Name(), toolbelt::LogLevel::kError, "%s",
                      eventStatus.ToString().c_str());
          return;
        }

        int status = vproc->WaitForZygoteNotification(c2);
        zygote->RemoveVirtualProcess(vproc);

        if (absl::Status status = proc->RemoveFromCgroup(proc->GetPid()); !status.ok()) {
          client->Log(proc->Name(), toolbelt::LogLevel::kError,
                      "Failed to remove process %s from cgroup: %s",
                      proc->Name().c_str(), status.ToString().c_str());
        }

        bool signaled = WIFSIGNALED(status);
        bool exited = WIFEXITED(status);
        int term_sig = WTERMSIG(status);
        int exit_status = WEXITSTATUS(status);
        // Can't be both exit and signal, but can be neither in the case
        // of a stop.  We don't expect anything to be stopped and don't
        // support it.
        if (!signaled && !exited) {
          signaled = true;
        }
        if (exited) {
          client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                      "Virtual process %s exited with status %d",
                      proc->Name().c_str(), exit_status);
        } else {
          client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                      "Virtual process %s received signal %d \"%s\"",
                      proc->Name().c_str(), term_sig, strsignal(term_sig));
        }
        if (absl::Status eventStatus = client->SendProcessStopEvent(
                proc->GetId(), !signaled, exit_status, term_sig);
            !eventStatus.ok()) {
          client->Log(proc->Name(), toolbelt::LogLevel::kError, "%s\n",
                      eventStatus.ToString().c_str());
          return;
        }
      }));

  return absl::OkStatus();
}

int VirtualProcess::Wait() {
  int e = SafeKill(pid_, 0);
  if (e == -1) {
    running_ = false;
    return 127;
  }
  return 0;
}

int VirtualProcess::WaitForZygoteNotification(co::Coroutine *c) {
  int status;
  c->Wait(notify_pipe_.ReadFd().Fd());
  ssize_t n = read(notify_pipe_.ReadFd().Fd(), &status, sizeof(status));
  if (n <= 0) {
    // EOF from notification means zygote is no longer running.
    (void)Stop(c);
    return 127;
  }
  if (absl::Status status = client_->GetStageZero().RemoveVirtualProcess(pid_);
      !status.ok()) {
    client_->Log(Name(), toolbelt::LogLevel::kError,
                 "Failed to remove virtual process %d", pid_);
  }
  if (n == sizeof(status)) {
    return static_cast<int>(status);
  }
  return 127; // Something to say that we have a problem.
}

absl::Status Process::AddToCgroup(int pid) {
  if (cgroup_.empty()) {
    return absl::OkStatus();
  }
  return stagezero::AddToCgroup(Name(), cgroup_, pid);
}

absl::Status Process::RemoveFromCgroup(int pid) {
  if (cgroup_.empty()) {
    return absl::OkStatus();
  }
  return stagezero::RemoveFromCgroup(Name(), cgroup_, pid);
}

} // namespace adastra::stagezero
