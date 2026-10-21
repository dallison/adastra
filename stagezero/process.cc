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
#include "toolbelt/pipe.h"

#include <ctype.h>
#include <syslog.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <fcntl.h>
#include <limits.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <vector>
#if defined(__linux__)
#include <linux/sched.h>
#include <linux/wait.h> // For P_PIDFD
#include <pty.h>
#include <sched.h>
#include <syscall.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <sched.h>
#include <sys/prctl.h>
#elif defined(__APPLE__)
#include <util.h>
#else
#error "Unknown OS"
#endif
#include <grp.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <termios.h>

#if HAVE_PIDFD
#include <syscall.h>
static int pidfd_open(pid_t pid, unsigned int flags) {
  return syscall(__NR_pidfd_open, pid, flags);
}

#endif

namespace adastra::stagezero {

// Unblocks all signals and resets every disposition to its default.  Used in
// a forked child before exec.
static absl::Status ResetSignals() {
  sigset_t empty_mask;
  sigemptyset(&empty_mask);
  if (sigprocmask(SIG_SETMASK, &empty_mask, nullptr) == -1) {
    return absl::InternalError(
        absl::StrFormat("Failed to set signal mask: %s", strerror(errno)));
  }

  struct sigaction default_action = {};
  default_action.sa_handler = SIG_DFL;
  sigemptyset(&default_action.sa_mask);
  for (int sig = 1; sig < NSIG; sig++) {
    if (sig == SIGKILL || sig == SIGSTOP) {
      continue;
    }
    // EINVAL is returned for signal numbers that can't be changed.
    if (sigaction(sig, &default_action, nullptr) == -1 && errno != EINVAL) {
      return absl::InternalError(
          absl::StrFormat("Failed to reset disposition for signal %d: %s", sig,
                          strerror(errno)));
    }
  }
  return absl::OkStatus();
}

Process::Process(co::CoroutineScheduler &scheduler, StageZero &stagezero,
                 std::shared_ptr<ClientHandler> client, std::string name)
    : scheduler_(scheduler), stagezero_(stagezero), client_(std::move(client)),
      name_(std::move(name)), local_symbols_(client_->GetGlobalSymbols()),
      local_parameters_(true) {
  // Add a locall symbol "name" for the process name.
  local_symbols_.AddSymbol("name", name_, false);
}

void Process::SetProcessId() {
  process_id_ = absl::StrFormat("%s/%s@%s:%d", client_->GetClientName(), name_,
                                client_->GetCompute(), pid_);
}

void Process::KillNow() {
  client_->Log(Name(), toolbelt::LogLevel::kInfo,
               "Killing process %s with pid %d", Name().c_str(), pid_);
  if (pid_ <= 0) {
    return;
  }
  int e = SafeKill(GetProcessGroupId(), SIGKILL);
  if (e != 0) {
    client_->Log(Name(), toolbelt::LogLevel::kError,
                 "Failed to send SIGKILL to %s: pid: %d: %s", Name().c_str(),
                 GetPid(), strerror(errno));
  }
  (void)client_->RemoveProcess(this);
}

int Process::WaitLoop(co::Coroutine *c,
                      std::optional<std::chrono::seconds> timeout) {
#if defined(__linux__) && HAVE_PIDFD
  toolbelt::FileDescriptor pidfd(dup(pid_fd_.Fd()));
  c->Wait(pidfd.Fd()); // Timeout ignored on linux.
  // We can't really get here unless the process has exited.
  running_ = false;
  siginfo_t siginfo;
  // Processes in namespaces are created by clone3 with no exit signal, so
  // __WALL is needed to reap them.
  int e = waitid(idtype_t(P_PIDFD), pid_fd_.Fd(), &siginfo,
                 WEXITED | WNOHANG | __WALL);
  if (e != 0) {
    return 127;
  }
  switch (siginfo.si_code) {
  case CLD_EXITED:
    return (siginfo.si_status & 0xff) << 8;
  case CLD_KILLED:
    return siginfo.si_status;
  }
  return 127;

#else
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
#endif
}

const std::shared_ptr<StreamInfo> Process::FindNotifyStream() const {
  for (auto &stream : streams_) {
    if (stream->disposition == proto::StreamControl::NOTIFY) {
      return stream;
    }
  }
  return nullptr;
}

const std::shared_ptr<StreamInfo>
Process::FindParametersStream(bool read) const {
  for (auto &stream : streams_) {
    if (stream->disposition == (read
                                    ? proto::StreamControl::PARAMETERS_READ
                                    : proto::StreamControl::PARAMETERS_WRITE)) {
      return stream;
    }
  }
  return nullptr;
}

const std::shared_ptr<StreamInfo>
Process::FindTelemetryStream(bool read) const {
  for (auto &stream : streams_) {
    if (stream->disposition == (read ? proto::StreamControl::TELEMETRY_READ
                                     : proto::StreamControl::TELEMETRY_WRITE)) {
      return stream;
    }
  }
  return nullptr;
}

StaticProcess::StaticProcess(
    co::CoroutineScheduler &scheduler, StageZero &stagezero,
    std::shared_ptr<ClientHandler> client,
    const stagezero::control::LaunchStaticProcessRequest &&req)
    : Process(scheduler, stagezero, std::move(client), req.opts().name()),
      req_(std::move(req)) {
  for (auto &var : req.opts().vars()) {
    local_symbols_.AddSymbol(var.name(), var.value(), var.exported());
  }
  for (auto &parameter : req.opts().parameters()) {
    parameters::Value v;
    v.FromProto(parameter.value());
    if (absl::Status status =
            local_parameters_.SetParameter(parameter.name(), v);
        !status.ok()) {
      client_->Log(Name(), toolbelt::LogLevel::kError,
                   "Failed to set local parameter %s: %s",
                   parameter.name().c_str(), status.ToString().c_str());
    }
  }
  SetSignalTimeouts(req.opts().sigint_shutdown_timeout_secs(),
                    req.opts().sigterm_shutdown_timeout_secs(),
                    req.opts().telemetry_shutdown_timeout_secs());
  SetUserAndGroup(req.opts().user(), req.opts().group());
  SetCgroup(req.opts().cgroup());
  SetDetached(req.opts().detached());
  if (req.opts().has_ns()) {
    Namespace n;
    n.FromProto(req.opts().ns());
    SetNamespace(std::move(n));
  }

  interactive_ = req.opts().interactive();
  if (req.opts().has_interactive_terminal()) {
    interactive_terminal_.FromProto(req.opts().interactive_terminal());
  }
  critical_ = req.opts().critical();
} // namespace adastra::stagezero

absl::Status StaticProcess::Start(co::Coroutine *c) {
  return StartInternal({}, true);
}

absl::Status
StaticProcess::StartInternal(const std::vector<std::string> extra_env_vars,
                             bool send_start_event) {
  if (absl::Status status = ValidateStreams(req_.streams()); !status.ok()) {
    return status;
  }
  if (interactive_ && detached_) {
    return absl::InternalError(
        "Cannot start an interactive process in detached mode");
  }
  if (absl::Status status = ForkAndExec(extra_env_vars); !status.ok()) {
    return status;
  }
  // Generate process id.
  SetProcessId();

  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [proc = shared_from_this(), client = client_,
       send_start_event](co::Coroutine *c) mutable {
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
              send_start_event = false;
            }
            // Read the data from the notify pipe.
            int64_t val;
            ssize_t n = read(notify_fd, &val, 8);
            if (n <= 0) {
              client->Log(proc->Name(), toolbelt::LogLevel::kError,
                          "Failed to read from notify pipe: %s",
                          strerror(errno));
              send_start_event = false;
            } else {
              client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                          "Process %s notified us of startup",
                          proc->Name().c_str());
            }
          }
        }
        // Send start event to client.
        if (send_start_event) {
          absl::Status eventStatus;
          if (proc->IsDetached()) {
            eventStatus =
                proc->GetStageZero().SendProcessStartEvent(proc->GetId());
          } else {
            eventStatus = client->SendProcessStartEvent(proc->GetId());
          }
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
        absl::Status eventStatus;
        if (proc->IsDetached()) {
          eventStatus = proc->GetStageZero().SendProcessStopEvent(
              proc->GetId(), !signaled, exit_status, term_sig);
        } else {
          eventStatus = client->SendProcessStopEvent(proc->GetId(), !signaled,
                                                     exit_status, term_sig);
        }
        if (!eventStatus.ok()) {
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
  toolbelt::FileDescriptor streamfd(dup(fd));
  for (;;) {
    int wait_fd = c->Wait(streamfd.Fd(), POLLIN);
    if (wait_fd != streamfd.Fd()) {
      return absl::InternalError("Interrupted");
    }
    ssize_t n = ::read(streamfd.Fd(), buffer, sizeof(buffer));
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
    if (stream->direction == proto::StreamControl::DEFAULT) {
      stream->direction = proto::StreamControl::INPUT;
    }
    break;
  case STDOUT_FILENO:
  case STDERR_FILENO:
    if (stream->direction == proto::StreamControl::DEFAULT) {
      stream->direction = proto::StreamControl::OUTPUT;
    }
    break;
  }
}

absl::Status Process::BuildStreams(
    const google::protobuf::RepeatedPtrField<proto::StreamControl> &streams,
    bool notify, bool telemetry) {
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

  // Open parameters streams.  These are from the point of view of the process.
  {
    auto rstream = std::make_shared<StreamInfo>();
    rstream->disposition = proto::StreamControl::PARAMETERS_READ;
    rstream->direction = proto::StreamControl::INPUT;
    absl::StatusOr<toolbelt::Pipe> rpipe = MakeFileDescriptors(false, nullptr);
    if (!rpipe.ok()) {
      return rpipe.status();
    }
    rstream->fd = rpipe->ReadFd().Fd();
    rstream->pipe = std::move(*rpipe);
    streams_.push_back(rstream);

    auto wstream = std::make_shared<StreamInfo>();
    wstream->disposition = proto::StreamControl::PARAMETERS_WRITE;
    wstream->direction = proto::StreamControl::OUTPUT;
    absl::StatusOr<toolbelt::Pipe> wpipe = MakeFileDescriptors(false, nullptr);
    if (!wpipe.ok()) {
      return wpipe.status();
    }
    wstream->fd = wpipe->WriteFd().Fd();
    wstream->pipe = std::move(*wpipe);
    streams_.push_back(wstream);

    // Events stream.
    auto estream = std::make_shared<StreamInfo>();
    estream->disposition = proto::StreamControl::PARAMETERS_EVENTS;
    estream->direction = proto::StreamControl::INPUT;
    absl::StatusOr<toolbelt::Pipe> epipe = MakeFileDescriptors(false, nullptr);
    if (!epipe.ok()) {
      return epipe.status();
    }
    parameters_event_write_fd_ = epipe->WriteFd();
    parameters_event_read_fd_ = epipe->ReadFd();
    estream->fd = epipe->WriteFd().Fd();
    estream->pipe = std::move(*epipe);
    streams_.push_back(estream);
  }

  // Open telemetry streams.  These are from the point of view of the process.
  if (telemetry) {
    auto rstream = std::make_shared<StreamInfo>();
    rstream->disposition = proto::StreamControl::TELEMETRY_READ;
    rstream->direction = proto::StreamControl::INPUT;
    absl::StatusOr<toolbelt::Pipe> rpipe = MakeFileDescriptors(false, nullptr);
    if (!rpipe.ok()) {
      return rpipe.status();
    }
    rstream->fd = rpipe->ReadFd().Fd();
    rstream->pipe = std::move(*rpipe);
    streams_.push_back(rstream);

    auto wstream = std::make_shared<StreamInfo>();
    wstream->disposition = proto::StreamControl::TELEMETRY_WRITE;
    wstream->direction = proto::StreamControl::OUTPUT;
    absl::StatusOr<toolbelt::Pipe> wpipe = MakeFileDescriptors(false, nullptr);
    if (!wpipe.ok()) {
      return wpipe.status();
    }
    wstream->fd = wpipe->WriteFd().Fd();
    wstream->pipe = std::move(*wpipe);
    streams_.push_back(wstream);
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
        scheduler_, [proc = shared_from_this(), client = client_,
                     this_end](co::Coroutine *c) {
          absl::Status status = StreamFromFileDescriptor(
              this_end,
              [proc, client](const char *buf, size_t len) -> absl::Status {
                // Write to client using an event.
                return client->SendOutputEvent(proc->Name(), proc->GetId(),
                                               STDOUT_FILENO, buf, len);
              },
              c);
          if (!status.ok()) {
            client->Log(proc->Name(), toolbelt::LogLevel::kError,
                        "Failed to read from stream: %s",
                        status.ToString().c_str());
          }
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
            scheduler_, [proc = shared_from_this(), stream,
                         client = client_](co::Coroutine *c) {
              absl::Status status = StreamFromFileDescriptor(
                  stream->pipe.ReadFd().Fd(),
                  [proc, stream, client](const char *buf,
                                         size_t len) -> absl::Status {
                    // Write to client using an event.
                    return client->SendOutputEvent(proc->Name(), proc->GetId(),
                                                   stream->fd, buf, len);
                  },
                  c);
              if (!status.ok()) {
                client->Log(proc->Name(), toolbelt::LogLevel::kError,
                            "Failed to read from stream: %s",
                            status.ToString().c_str());
              }
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
          scheduler_, [proc = shared_from_this(), stream,
                       client = client_](co::Coroutine *c) {
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
            if (!status.ok()) {
              client->Log(proc->Name(), toolbelt::LogLevel::kError,
                          "Failed to read from stream: %s",
                          status.ToString().c_str());
            }
          }));

      break;
    }
    case proto::StreamControl::SYSLOG: {
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
          scheduler_, [proc = shared_from_this(), stream,
                       client = client_](co::Coroutine *c) {
            absl::Status status = StreamFromFileDescriptor(
                stream->pipe.ReadFd().Fd(),
                [proc, stream, client](const char *buf,
                                       size_t len) -> absl::Status {
                  syslog(stream->fd == 1 ? LOG_INFO : LOG_ERR, "%s: %s",
                         proc->Name().c_str(), std::string(buf, len).c_str());
                  return absl::OkStatus();
                },
                c);
            if (!status.ok()) {
              client->Log(proc->Name(), toolbelt::LogLevel::kError,
                          "Failed to read from stream: %s",
                          status.ToString().c_str());
            }
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

void Process::RunParameterServer() {
  std::shared_ptr<StreamInfo> param_read_stream = FindParametersStream(true);
  std::shared_ptr<StreamInfo> param_write_stream = FindParametersStream(false);
  assert(param_read_stream != nullptr && param_write_stream != nullptr);

  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_, [proc = shared_from_this(), param_read_stream,
                   param_write_stream, client = client_](co::Coroutine *c) {
        toolbelt::FileDescriptor &rfd = param_write_stream->pipe.ReadFd();
        toolbelt::FileDescriptor &wfd = param_read_stream->pipe.WriteFd();
        while (rfd.Valid() && wfd.Valid()) {
          adastra::proto::parameters::Request req;
          {
            uint32_t len;
            int wait_fd = c->Wait(rfd.Fd(), POLLIN);
            if (wait_fd != rfd.Fd()) {
              return;
            }
            ssize_t n = ::read(rfd.Fd(), &len, sizeof(len));
            if (n <= 0) {
              return;
            }
            std::vector<char> buffer(len);
            char *buf = buffer.data();
            size_t remaining = len;
            while (remaining > 0) {
              int wait_fd = c->Wait(rfd.Fd(), POLLIN);
              if (wait_fd != rfd.Fd()) {
                return;
              }
              ssize_t n = ::read(rfd.Fd(), buf, remaining);
              if (n <= 0) {
                return;
              }
              remaining -= n;
              buf += n;
            }

            if (!req.ParseFromArray(buffer.data(), buffer.size())) {
              client->Log(proc->Name(), toolbelt::LogLevel::kError,
                          "Failed to parse parameter stream message");
              break;
            }
          }
          adastra::proto::parameters::Response resp;

          proc->GetStageZero().HandleParameterServerRequest(proc, req, resp,
                                                            client, c);

          uint64_t len = resp.ByteSizeLong();
          std::vector<char> resp_buffer(len + sizeof(uint32_t));
          char *respbuf = resp_buffer.data() + 4;
          if (!resp.SerializeToArray(respbuf, uint32_t(len))) {
            client->Log(proc->Name(), toolbelt::LogLevel::kError,
                        "Failed to serialize parameters response");
            break;
          }
          // Copy length into buffer.
          memcpy(resp_buffer.data(), &len, sizeof(uint32_t));

          // Write back to pipe in a loop.
          char *buf = resp_buffer.data();
          size_t remaining = len + sizeof(uint32_t);
          while (remaining > 0) {
            int wait_fd = c->Wait(wfd.Fd(), POLLOUT);
            if (wait_fd != wfd.Fd()) {
              return;
            }
            ssize_t n = ::write(wfd.Fd(), buf, remaining);
            if (n <= 0) {
              return;
            }
            remaining -= n;
            buf += n;
          }
        }
      }));
}

void Process::RunTelemetryServer() {
  std::shared_ptr<StreamInfo> tele_write_stream = FindTelemetryStream(false);
  assert(tele_write_stream != nullptr);

  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_, [proc = shared_from_this(), tele_write_stream,
                   client = client_](co::Coroutine *c) {
        toolbelt::FileDescriptor &rfd = tele_write_stream->pipe.ReadFd();
        while (rfd.Valid()) {
          adastra::proto::telemetry::Status status;
          {
            uint32_t len;
            int wait_fd = c->Wait(rfd.Fd(), POLLIN);
            if (wait_fd != rfd.Fd()) {
              return;
            }
            ssize_t n = ::read(rfd.Fd(), &len, sizeof(len));
            if (n <= 0) {
              return;
            }
            std::vector<char> buffer(len);
            char *buf = buffer.data();
            size_t remaining = len;
            while (remaining > 0) {
              int wait_fd = c->Wait(rfd.Fd(), POLLIN);
              if (wait_fd != rfd.Fd()) {
                return;
              }
              ssize_t n = ::read(rfd.Fd(), buf, remaining);
              if (n <= 0) {
                return;
              }
              remaining -= n;
              buf += n;
            }

            if (!status.ParseFromArray(buffer.data(), buffer.size())) {
              client->Log(proc->Name(), toolbelt::LogLevel::kError,
                          "Failed to parse telemetry status message");
              break;
            }

            proc->GetStageZero().HandleTelemetryServerStatus(proc, status,
                                                             client, c);
          }
        }
      }));
}

void Process::SendParameterUpdateEvent(const std::string &name,
                                       const parameters::Value &value,
                                       co::Coroutine *c) {
  if (!wants_parameter_events_) {
    return;
  }
  adastra::proto::parameters::ParameterEvent event;
  event.mutable_update()->set_name(name);
  value.ToProto(event.mutable_update()->mutable_value());
  SendParameterEvent(event, c);
}

void Process::SendParameterDeleteEvent(const std::string &name,
                                       co::Coroutine *c) {
  if (!wants_parameter_events_) {
    return;
  }
  adastra::proto::parameters::ParameterEvent event;
  event.set_delete_(name);
  SendParameterEvent(event, c);
}

void Process::SendParameterEvent(
    const adastra::proto::parameters::ParameterEvent &event, co::Coroutine *c) {
  uint64_t len = event.ByteSizeLong();
  std::vector<char> event_buffer(len + sizeof(uint32_t));
  char *eventbuf = event_buffer.data() + 4;
  if (!event.SerializeToArray(eventbuf, uint32_t(len))) {
    client_->Log(Name(), toolbelt::LogLevel::kError,
                 "Failed to serialize parameters event");
    return;
  }
  // Copy length into buffer.
  memcpy(event_buffer.data(), &len, sizeof(uint32_t));
  char *buf = event_buffer.data();
  size_t remaining = len + sizeof(uint32_t);
  while (remaining > 0) {
    int wait_fd = c->Wait(parameters_event_write_fd_.Fd(), POLLOUT);
    if (wait_fd != parameters_event_write_fd_.Fd()) {
      return;
    }
    ssize_t n = ::write(parameters_event_write_fd_.Fd(), buf, remaining);
    if (n <= 0) {
      return;
    }
    remaining -= n;
    buf += n;
  }
}

absl::Status
StaticProcess::ForkAndExec(const std::vector<std::string> extra_env_vars) {
  // It is better to detect things like missing files earlier so that we
  // can give a nice error.  If we wait until after the fork, there is
  // no way to return an error to the client and it will be reported as events.

  struct stat st;
  std::string exe = local_symbols_.ReplaceSymbols(req_.proc().executable());

  int e = ::stat(exe.c_str(), &st);
  if (e == -1) {
    // Can't find the executable
    return absl::InternalError(
        absl::StrFormat("Can't find executable %s", exe));
  }
  // Executable found, make sure it's executable.
  if (!S_ISREG(st.st_mode) ||
      (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
    return absl::InternalError(
        absl::StrFormat("%s is not a file or is not executable", exe));
  }

  // Set up streams.
  if (absl::Status status = BuildStreams(req_.streams(), req_.opts().notify(),
                                         req_.opts().telemetry());
      !status.ok()) {
    return status;
  }
  uid_t uid = geteuid();
  gid_t gid = getegid();
#if defined(__APPLE__)
  std::vector<int> groups;
#else
  std::vector<gid_t> groups;
#endif
  if (!user_.empty()) {
    struct passwd *p = getpwnam(user_.c_str());
    if (p == nullptr) {
      return absl::InternalError(absl::StrFormat("Unknown user %s", user_));
    }
    uid = p->pw_uid;
    gid = p->pw_gid;
    int  ngroups{0};
    getgrouplist(user_.c_str(), gid, nullptr, &ngroups);
    if (ngroups > 0) {
      groups.resize(ngroups);
      if (getgrouplist(user_.c_str(), gid,
                       groups.data(),
                       &ngroups) == -1) {
        // -1 means ngroups was too small to store all groups
        // But we just queried the number of necessary groups, and this should
        // not be changing
        return absl::InternalError(absl::StrFormat(
            "Failed to determine all groups associated with user '%s' ('%s')",
            user_, strerror(errno)));
      }
    }
  }

  if (!group_.empty()) {
    struct group *g = getgrnam(group_.c_str());
    if (g == nullptr) {
      return absl::InternalError(absl::StrFormat("Unknown group %s", group_));
    }
    gid = g->gr_gid;
  }

  // Run a coroutine to process commands from the parameter stream.
  RunParameterServer();
  if (UseTelemetry()) {
    RunTelemetryServer();
  }

  absl::StatusOr<toolbelt::Pipe> startup_pipe = toolbelt::Pipe::Create();
  if (!startup_pipe.ok()) {
    return startup_pipe.status();
  }

#if defined(__linux__)
  // On Linux we can use clone3 instead of fork if we have any namespace
  // assignments.
  if (ns_.has_value()) {
    struct clone_args args = {
        .flags = static_cast<uint64_t>(ns_->CloneType()),
        // All other members are zero.
    };
    pid_ = syscall(__NR_clone3, &args, sizeof(args));
  } else {
    pid_ = fork();
  }
#else
  pid_ = fork();
#endif
  if (pid_ == -1) {
    return absl::InternalError(
        absl::StrFormat("Fork failed: %s", strerror(errno)));
  }
  if (pid_ == 0) {
    // The parent will get EOF if we exit before writing to the pipe.
    startup_pipe->ReadFd().Close();

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
          _exit(1);
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
        if (stream->disposition != proto::StreamControl::NOTIFY &&
            stream->disposition != proto::StreamControl::PARAMETERS_READ &&
            stream->disposition != proto::StreamControl::PARAMETERS_WRITE &&
            stream->disposition != proto::StreamControl::PARAMETERS_EVENTS &&
            stream->disposition != proto::StreamControl::TELEMETRY_READ &&
            stream->disposition != proto::StreamControl::TELEMETRY_WRITE) {
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
              _exit(1);
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
            _exit(1);
          }

          if (!stream->term_name.empty()) {
            // Set the TERM environment variable to the terminal name given.
            local_symbols_.AddSymbol("TERM", stream->term_name, true);
          }

          // Close the duplicated fd.
          fd.Reset();
        }

        if (stream->disposition == proto::StreamControl::CLIENT ||
            stream->disposition == proto::StreamControl::NOTIFY ||
            stream->disposition == proto::StreamControl::PARAMETERS_READ ||
            stream->disposition == proto::StreamControl::PARAMETERS_WRITE ||
            stream->disposition == proto::StreamControl::PARAMETERS_EVENTS ||
            stream->disposition == proto::StreamControl::TELEMETRY_READ ||
            stream->disposition == proto::StreamControl::TELEMETRY_WRITE) {
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

    // Tell the process what the parameters stream is.  This is used by the
    // process to read and modify the global and local parameters for the
    // system. Think ROS parameter server.  The directions here are from the
    // point of view of the process.  So the read stream is the stream the
    // process reads from.
    std::shared_ptr<StreamInfo> params_read_stream = FindParametersStream(true);
    std::shared_ptr<StreamInfo> params_write_stream =
        FindParametersStream(false);
    assert(params_read_stream != nullptr);
    assert(params_write_stream != nullptr);
    env_strings.push_back(
        absl::StrFormat("STAGEZERO_PARAMETERS_FDS=%d:%d:%d",
                        params_read_stream->pipe.ReadFd().Fd(),
                        params_write_stream->pipe.WriteFd().Fd(),
                        parameters_event_read_fd_.Fd()));

    if (UseTelemetry()) {
      // Tell the process what the telemetry stream is.
      std::shared_ptr<StreamInfo> tele_read_stream = FindTelemetryStream(true);
      std::shared_ptr<StreamInfo> tele_write_stream =
          FindTelemetryStream(false);
      assert(tele_read_stream != nullptr);
      assert(tele_write_stream != nullptr);
      env_strings.push_back(absl::StrFormat(
          "STAGEZERO_TELEMETRY_FDS=%d:%d", tele_read_stream->pipe.ReadFd().Fd(),
          tele_write_stream->pipe.WriteFd().Fd()));
    }

    absl::flat_hash_map<std::string, Symbol *> env_vars =
        local_symbols_.GetEnvironmentSymbols();
    for (auto &[name, symbol] : env_vars) {
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

    if (!interactive_) {
      // Can't set a process group on a new session.
      int perr = setpgid(0, 0);
      if (perr == -1) {
        std::cerr << "Failed to set process group: " << strerror(errno)
                  << std::endl;
        _exit(1);
      }
    }

    // Add the process to the cgroup if it is set.
    if (absl::Status status = AddToCgroup(getpid()); !status.ok()) {
      std::cerr << "Failed to add process to cgroup " << cgroup_ << status
                << std::endl;
      _exit(1);
    }

    if (geteuid() == 0) {
#if defined(__linux__)
      // Set the kernel scheduler if running as root.
      if (kernel_scheduler_policy_.policy !=
          KernelSchedulerPolicyType::kDefault) {
        int policy = 0;

        switch (kernel_scheduler_policy_.policy) {
        case KernelSchedulerPolicyType::kDefault:
          break;
        case KernelSchedulerPolicyType::kBatch:
          policy = SCHED_BATCH;
          break;
        case KernelSchedulerPolicyType::kIdle:
          policy = SCHED_IDLE;
          break;
        case KernelSchedulerPolicyType::kFifo:
          policy = SCHED_FIFO;
          break;
        case KernelSchedulerPolicyType::kRoundRobin:
          policy = SCHED_RR;
          break;
        }
        if (kernel_scheduler_policy_.reset_on_fork) {
          policy |= SCHED_RESET_ON_FORK;
        }
        struct sched_param param = {0};
        param.sched_priority = kernel_scheduler_policy_.priority;
        int e = sched_setscheduler(0, policy, &param);
        if (e == -1) {
          std::cerr << "Failed to set scheduler policy: " << strerror(errno)
                    << std::endl;
          _exit(1);
        }
      }

      // Set the CPU affinity.
      if (!cpus_.empty()) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        for (int cpu : cpus_) {
          CPU_SET(cpu, &cpuset);
        }

        e = sched_setaffinity(0, sizeof(cpuset), &cpuset);
        if (e == -1) {
          std::cerr << "Failed to set CPU affinity: " << strerror(errno)
                    << std::endl;
          cpu_set_t ccpuset;

          int e2 = sched_getaffinity(0, sizeof(cpuset), &ccpuset);
          if (e2 == 0) {
            for (int i = 0; i < CPU_SETSIZE; i++) {
              if (CPU_ISSET(i, &ccpuset)) {
                std::cerr << "CPU " << i << " is available" << std::endl;
              }
            }
          }
          _exit(1);
        }
      }

      // Need to be able to set the use and group ids here.  We will drop the
      // caps if the process doesn't want them.
      bool drop_uid_cap = false;
      bool drop_gid_cap = false;
      adastra::Caps current_caps;

      // Capabilities, only if we are not going to runs as root.
      // We drop all the capabilities and then add the ones we want.
      if (uid != 0 && !capabilities_.capabilities.empty()) {
        int e = prctl(PR_SET_SECUREBITS, SECBIT_NO_SETUID_FIXUP, 0, 0, 0);
        if (e == -1) {
          std::cerr << "Failed to set secure bits: " << strerror(errno)
                    << std::endl;
          _exit(1);
        }

        current_caps.Modify(CAP_SETGID);
        current_caps.Modify(CAP_SETUID);
        drop_gid_cap = true;
        drop_uid_cap = true;

        for (auto &cap : capabilities_.capabilities) {
          int cap_number = 0;
          auto number_or_status =
              adastra::CapabilitySet::CapabilityFromString(cap);
          if (number_or_status.ok()) {
            cap_number = *number_or_status;
          } else {
            // Allow #<int> for a capability number for which we don't have a
            // name.
            if (cap[0] == '#') {
              int num = 0;
              bool ok = absl::SimpleAtoi(cap.substr(1), &num);
              if (!ok) {
                std::cerr << "Malformed capability number " << cap
                          << std::endl;
                _exit(1);
              }
              cap_number = num;
            } else {
              std::cerr << "Unknown capability " << cap << std::endl;
              // Print all caps.
              for (auto &c : capabilities_.capabilities) {
                std::cerr << "  Capability " << c << std::endl;
              }
              continue;
            }
          }

          if (cap_number == CAP_SETUID) {
            drop_uid_cap = false;
          }
          if (cap_number == CAP_SETGID) {
            drop_gid_cap = false;
          }
          current_caps.Modify(cap_number);
        }

        if (absl::Status status = current_caps.Set(); !status.ok()) {
          std::cerr << "Failed to set capabilities: " << status.ToString()
                    << std::endl;
          _exit(1);
        }
      }

      // We can only set the user and group if we are running as root.
      if (!groups.empty()) {
        e = setgroups(groups.size(), groups.data());
        if (e == -1) {
          std::cerr << "Failed to setgroups: " << strerror(errno) << std::endl;
          _exit(1);
        }
      }
#endif

      // We can only set the user and group if we are running as root.
      e = setuid(uid);
      if (e == -1) {
        std::cerr << "Failed to setuid: " << strerror(errno) << std::endl;
        _exit(1);
      }
      e = setgid(gid);
      if (e == -1) {
        std::cerr << "Failed to setgid: " << strerror(errno) << std::endl;
        _exit(1);
      }
#if defined(__linux__)
      if (drop_gid_cap || drop_uid_cap) {
        if (drop_gid_cap) {
          // Drop the setgid capability.
          current_caps.Modify(CAP_SETGID, false);
        }
        if (drop_uid_cap) {
          // Drop the setuid capability.
          current_caps.Modify(CAP_SETUID, false);
        }
        if (absl::Status status = current_caps.Set(); !status.ok()) {
          std::cerr << "Failed to drop id caps: " << status.ToString()
                    << std::endl;
          _exit(1);
        }
      }
#endif
    }

    // Don't let the process inherit StageZero's signal mask or ignored
    // signals (StageZero ignores SIGPIPE).
    if (absl::Status status = ResetSignals(); !status.ok()) {
      std::cerr << "Failed to reset signals: " << status << std::endl;
      _exit(1);
    }

    int64_t val = 1;
    ssize_t n = ::write(startup_pipe->WriteFd().Fd(), &val, sizeof(val));
    if (n != sizeof(val)) {
      std::cerr << "Failed to write to startup pipe: " << strerror(errno)
                << std::endl;
      _exit(1);
    }
    startup_pipe->WriteFd().Close();
     (exe.c_str(),
           reinterpret_cast<char *const *>(const_cast<char **>(argv.data())),
           reinterpret_cast<char *const *>(const_cast<char **>(env.data())));
    std::cerr << "Failed to exec " << argv[0] << ": " << strerror(errno)
              << std::endl;
    _exit(1);
  }
  startup_pipe->WriteFd().Close();
#if defined(__linux__) && HAVE_PIDFD
  int pidfd = pidfd_open(pid_, 0);
  if (pidfd == -1) {
    return absl::InternalError(absl::StrFormat(
        "Failed to open pidfd for pid %d: %s", pid_, strerror(errno)));
  }
  pid_fd_.SetFd(pidfd);
#endif

  // Close redirected stream fds in parent.
  if (interactive_) {
    // The proc end has been duplicated in the child and we don't need it
    // in the parent.
    interactive_proc_end_.Reset();
    // This end is what we read and write.
  }
  for (auto &stream : streams_) {
    if (stream->disposition != proto::StreamControl::CLOSE &&
        stream->disposition != proto::StreamControl::STAGEZERO) {
      toolbelt::FileDescriptor &fd =
          stream->direction == proto::StreamControl::OUTPUT
              ? stream->pipe.WriteFd()
              : stream->pipe.ReadFd();
      fd.Reset();
    }
  }
#if defined(__linux__)
  if (absl::Status status = BuildLinuxParameters(); !status.ok()) {
    return status;
  }
#endif

  int64_t val;
  ssize_t n = ::read(startup_pipe->ReadFd().Fd(), &val, sizeof(val));
  if (n != sizeof(val)) {
    if (n == 0) {
      // EOF means the child exited before writing to the pipe.
      return absl::InternalError(
          absl::StrFormat("Process %s (PID %d) failed to start", Name(), pid_));
    }
    return absl::InternalError(absl::StrFormat(
        "Failed to read from startup pipe: %s", strerror(errno)));
  }
  return absl::OkStatus();
}

int StaticProcess::Wait() {
  int status = 0;
  int wait_arg = WNOHANG;
#if defined(__linux__)
  // __WALL: see WaitLoop.
  wait_arg |= __WALL;
#endif
  pid_t pid = waitpid(pid_, &status, wait_arg);
  if (pid == -1) {
    if (errno == EINTR) {
      return 0;
    }
    // The process can never be reaped, so treat it as gone rather than
    // having WaitLoop poll it forever.
    client_->Log(Name(), toolbelt::LogLevel::kError,
                 "Failed to reap process %s (pid %d): %s", Name().c_str(), pid_,
                 strerror(errno));
    running_ = false;
    return 127;
  }
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

absl::Status
Process::SendTelemetryCommand(const adastra::proto::telemetry::Command &command,
                              co::Coroutine *c) {
  if (!UseTelemetry()) {
    client_->Log(Name(), toolbelt::LogLevel::kError,
                 "Telemetry is not enabled for process %s", Name().c_str());
    return absl::OkStatus();
  }
  // We write to the telemetry read stream since that process is reading from
  // it.
  std::shared_ptr<StreamInfo> tele_read_stream = FindTelemetryStream(true);
  assert(tele_read_stream != nullptr);

  toolbelt::FileDescriptor &wfd = tele_read_stream->pipe.WriteFd();

  uint64_t len = command.ByteSizeLong();
  std::vector<char> cmd_buffer(len + sizeof(uint32_t));
  char *cmdbuf = cmd_buffer.data() + 4;
  if (!command.SerializeToArray(cmdbuf, uint32_t(len))) {
    return absl::InternalError("Failed to serialize telemetry command");
  }
  // Copy length into buffer.
  memcpy(cmd_buffer.data(), &len, sizeof(uint32_t));

  // Write to pipe in a loop.
  char *buf = cmd_buffer.data();
  size_t remaining = len + sizeof(uint32_t);
  while (remaining > 0) {
    int wait_fd = c->Wait(wfd.Fd(), POLLOUT);
    if (wait_fd != wfd.Fd()) {
      break;
    }
    ssize_t n = ::write(wfd.Fd(), buf, remaining);
    if (n <= 0) {
      return absl::InternalError("Failed to write telemetry command");
    }
    remaining -= n;
    buf += n;
  }
  return absl::OkStatus();
}

absl::Status Process::SendTelemetryShutdown(int exit_code, int timeout_secs,
                                            co::Coroutine *c) {
  adastra::proto::telemetry::ShutdownCommand shutdown;
  shutdown.set_exit_code(exit_code);
  shutdown.set_timeout_seconds(timeout_secs);
  adastra::proto::telemetry::Command command;
  command.mutable_command()->PackFrom(shutdown);
  return SendTelemetryCommand(command, c);
}

absl::Status Process::Stop(co::Coroutine *c) {
  if (stopping_) {
    return absl::OkStatus();
  }
  stopping_ = true;
  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [proc = shared_from_this(), client = client_](co::Coroutine *c2) {
        if (!proc->IsRunning()) {
          return;
        }

        // If telemetry is enabled, send a shutdown command to the process.
        int timeout = proc->TelemetryShutdownTimeoutSecs();
        if (timeout > 0 && proc->UseTelemetry()) {
          client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                      "Sending shutdown telemetry command to process %s "
                      "(timeout %d seconds)",
                      proc->Name().c_str(), timeout);
          absl::Status status = proc->SendTelemetryShutdown(0, timeout, c2);
          if (!status.ok()) {
            client->Log(
                proc->Name(), toolbelt::LogLevel::kDebug,
                "Failed to send shutdown telemetry command to process %s: %s",
                proc->Name().c_str(), status.ToString().c_str());
          } else {
#if defined(__linux__) && HAVE_PIDFD
            int fd =
                c2->Wait(proc->GetPidFd().Fd(), POLLIN,
                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::seconds(timeout))
                             .count());
            if (fd != -1) {
              return;
            }
#else
            (void)proc->WaitLoop(c2, std::chrono::seconds(timeout));
#endif
            if (!proc->IsRunning()) {
              return;
            }
          }
        }

        timeout = proc->SigIntTimeoutSecs();
        if (timeout > 0) {
          client->Log(
              proc->Name(), toolbelt::LogLevel::kDebug,
              "Killing process %s (pid %d) with SIGINT (timeout %d seconds)",
              proc->Name().c_str(), proc->GetProcessGroupId(), timeout);
          int e = SafeKill(proc->GetProcessGroupId(), SIGINT);
          if (e != 0) {
            client->Log(proc->Name(), toolbelt::LogLevel::kError,
                        "Failed to send SIGINT to %s: pid: %d: %s",
                        proc->Name().c_str(), proc->GetPid(), strerror(errno));
          }
#if defined(__linux__) && HAVE_PIDFD
          int fd =
              c2->Wait(proc->GetPidFd().Fd(), POLLIN,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::seconds(timeout))
                           .count());
          if (fd != -1) {
            return;
          }
#else

          (void)proc->WaitLoop(c2, std::chrono::seconds(timeout));
#endif
          if (!proc->IsRunning()) {
            return;
          }
        }
        timeout = proc->SigTermTimeoutSecs();
        if (timeout > 0) {
          client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                      "Killing process %s with SIGTERM (timeout %d seconds)",
                      proc->Name().c_str(), timeout);
          int e = SafeKill(proc->GetProcessGroupId(), SIGTERM);
          if (e != 0) {
            client->Log(proc->Name(), toolbelt::LogLevel::kError,
                        "Failed to send SIGTERM to %s: pid: %d: %s",
                        proc->Name().c_str(), proc->GetPid(), strerror(errno));
          }

#if defined(__linux__) && HAVE_PIDFD
          c2->Wait(proc->GetPidFd().Fd(), POLLIN,
                   std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::seconds(timeout))
                       .count());
#else
          (void)proc->WaitLoop(c2, std::chrono::seconds(timeout));
#endif
        }

        // Always send SIGKILL if it's still running.  It can't ignore this.
        if (proc->IsRunning()) {
          client->Log(proc->Name(), toolbelt::LogLevel::kDebug,
                      "Killing process %s with SIGKILL", proc->Name().c_str());
          int e = SafeKill(proc->GetProcessGroupId(), SIGKILL);
          if (e != 0) {
            client->Log(proc->Name(), toolbelt::LogLevel::kError,
                        "Failed to send SIGKILL to %s: pid: %d: %s",
                        proc->Name().c_str(), proc->GetPid(), strerror(errno));
          }
        }
      },
      "Stopper"));
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
  // Spawn only applies virtual_process_group alongside virtual_process_user.
  if (req_.opts().virtual_process_user().empty() &&
      !req_.opts().virtual_process_group().empty()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Zygote %s sets virtual_process_group without virtual_process_user",
        Name()));
  }

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
  // The zygote connects by name, so neither it nor any other process we
  // launch needs to inherit the listening socket.
  if (absl::Status status = listen_socket.SetCloseOnExec(); !status.ok()) {
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
  auto acceptor = new co::Coroutine(scheduler_, [proc = shared_from_this(),
                                                 listen_socket =
                                                     std::move(listen_socket),
                                                 client = client_](
                                                    co::Coroutine *c) mutable {
    absl::StatusOr<toolbelt::UnixSocket> s = listen_socket.Accept(c);
    if (!s.ok()) {
      client->Log(proc->Name(), toolbelt::LogLevel::kError,
                  "Unable to accept connection from zygote %s: %s",
                  proc->Name().c_str(), s.status().ToString().c_str());
      return;
    }
    if (absl::Status status = s->SetCloseOnExec(); !status.ok()) {
      client->Log(proc->Name(), toolbelt::LogLevel::kError,
                  "Unable to set close-on-exec on zygote %s control socket: %s",
                  proc->Name().c_str(), status.ToString().c_str());
      return;
    }
    auto zygote = std::static_pointer_cast<Zygote>(proc);
    if (absl::Status status = zygote->OpenControlChannel(std::move(*s));
        !status.ok()) {
      client->Log(proc->Name(), toolbelt::LogLevel::kError,
                  "Unable to open control channel to zygote %s: %s",
                  proc->Name().c_str(), status.ToString().c_str());
      return;
    }
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
          // EOF or error means zygote is no longer running.
          zygote->HandleNotificationsClosed();
          return;
        }
        if (n == sizeof(pid_and_status) &&
            (pid_and_status & (1LL << 63)) != 0) {
          // Zygote notified us of a process going down.  This contains
          // both the process id and the status.
          int pid = (pid_and_status >> 32) & 0x7fffffff;
          int64_t status = pid_and_status & 0xffffffff;
          zygote->HandleExitNotification(pid, status);
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

// Processes forked while a trigger fd is open must not inherit it.
static absl::StatusOr<toolbelt::TriggerFd> CreateCloseOnExecTriggerFd() {
  absl::StatusOr<toolbelt::TriggerFd> trigger = toolbelt::TriggerFd::Create();
  if (!trigger.ok()) {
    return trigger.status();
  }
  if (absl::Status status = trigger->GetPollFd().SetCloseOnExec();
      !status.ok()) {
    return status;
  }
  return trigger;
}

// Carries a zygote response from the receiver coroutine to the coroutine
// waiting in Spawn.
struct Zygote::SpawnReply {
  SpawnReply(toolbelt::TriggerFd t, std::shared_ptr<VirtualProcess> p)
      : trigger(std::move(t)), process(std::move(p)) {}

  void Complete(SpawnResult r) {
    result = std::move(r);
    trigger.Trigger();
  }

  toolbelt::TriggerFd trigger;
  std::shared_ptr<VirtualProcess> process;
  std::optional<SpawnResult> result;
};

absl::Status Zygote::OpenControlChannel(toolbelt::UnixSocket s) {
  if (control_channel_open_) {
    return absl::InternalError(
        absl::StrFormat("Zygote %s already has a control channel", Name()));
  }
  absl::StatusOr<toolbelt::TriggerFd> trigger = CreateCloseOnExecTriggerFd();
  if (!trigger.ok()) {
    return trigger.status();
  }
  int write_fd = fcntl(s.GetFileDescriptor().Fd(), F_DUPFD_CLOEXEC, 0);
  if (write_fd == -1) {
    return absl::InternalError(
        absl::StrFormat("Failed to duplicate zygote %s control socket: %s",
                        Name(), strerror(errno)));
  }
  control_write_socket_ = toolbelt::UnixSocket(write_fd, /*connected=*/true);
  control_socket_ = std::move(s);
  outgoing_spawns_trigger_ = std::move(*trigger);
  control_channel_open_ = true;

  auto zygote = std::static_pointer_cast<Zygote>(shared_from_this());
  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [zygote](co::Coroutine *c) { zygote->ReceiveSpawnResponses(c); },
      absl::StrFormat("%s spawn receiver", Name())));
  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_, [zygote](co::Coroutine *c) { zygote->SendSpawnRequests(c); },
      absl::StrFormat("%s spawn sender", Name())));
  return absl::OkStatus();
}

void Zygote::CloseControlChannel(const std::string &reason) {
  if (!control_channel_open_) {
    return;
  }
  control_channel_open_ = false;
  client_->Log(Name(), toolbelt::LogLevel::kDebug,
               "Zygote %s control channel closed: %s", Name().c_str(),
               reason.c_str());
  // Shut the socket down rather than closing it: the other channel coroutine
  // may be waiting on it, and closing an fd that is in the epoll set removes
  // it without waking the waiter.
  (void)::shutdown(control_socket_.GetFileDescriptor().Fd(), SHUT_RDWR);
  outgoing_spawns_.clear();
  std::deque<std::shared_ptr<SpawnReply>> pending;
  pending.swap(pending_spawn_replies_);
  for (auto &reply : pending) {
    reply->Complete(absl::InternalError(absl::StrFormat(
        "Zygote %s control channel closed: %s", Name(), reason)));
  }
  early_exits_.clear();
  outgoing_spawns_trigger_.Trigger();
}

void Zygote::SendSpawnRequests(co::Coroutine *c) {
  while (control_channel_open_) {
    if (outgoing_spawns_.empty()) {
      outgoing_spawns_trigger_.Clear();
      c->Wait(outgoing_spawns_trigger_.GetPollFd().Fd(), POLLIN);
      continue;
    }
    // This is the only coroutine that writes to the socket, so a request and
    // its fds are never interleaved with another request even when a send has
    // to wait.
    OutgoingSpawn spawn = std::move(outgoing_spawns_.front());
    outgoing_spawns_.pop_front();
    char *buf = spawn.buffer.data() + sizeof(int32_t);
    size_t buflen = spawn.buffer.size() - sizeof(int32_t);
    if (absl::StatusOr<ssize_t> n =
            control_write_socket_.SendMessage(buf, buflen, c);
        !n.ok()) {
      CloseControlChannel(n.status().ToString());
      return;
    }
    if (absl::Status status = control_write_socket_.SendFds(spawn.fds, c);
        !status.ok()) {
      CloseControlChannel(status.ToString());
      return;
    }
  }
}

void Zygote::ReceiveSpawnResponses(co::Coroutine *c) {
  while (control_channel_open_) {
    absl::StatusOr<std::vector<char>> message =
        control_socket_.ReceiveVariableLengthMessage(c);
    if (!message.ok()) {
      CloseControlChannel(message.status().ToString());
      return;
    }
    if (message->empty()) {
      CloseControlChannel("Zygote closed its control socket");
      return;
    }
    std::vector<toolbelt::FileDescriptor> fds;
    if (absl::Status status = control_socket_.ReceiveFds(fds, c);
        !status.ok()) {
      CloseControlChannel(status.ToString());
      return;
    }
    control::SpawnResponse response;
    if (!response.ParseFromArray(message->data(),
                                 static_cast<int>(message->size()))) {
      CloseControlChannel("Failed to parse spawn response");
      return;
    }
    if (pending_spawn_replies_.empty()) {
      CloseControlChannel("Zygote sent a spawn response with no request");
      return;
    }
    std::shared_ptr<SpawnReply> reply =
        std::move(pending_spawn_replies_.front());
    pending_spawn_replies_.pop_front();
    reply->Complete(CompleteSpawn(response, std::move(fds), reply->process));
    if (pending_spawn_replies_.empty()) {
      // No in-flight spawn can claim these, and keeping them risks matching a
      // reused pid.
      early_exits_.clear();
    }
  }
}

Zygote::SpawnResult
Zygote::CompleteSpawn(const control::SpawnResponse &response,
                      std::vector<toolbelt::FileDescriptor> fds,
                      const std::shared_ptr<VirtualProcess> &process) {
  if (!response.error().empty()) {
    return absl::InternalError(response.error());
  }
  toolbelt::FileDescriptor pidfd;
#if defined(__linux__) && HAVE_PIDFD
  if (response.pidfd_index() < 0 ||
      size_t(response.pidfd_index()) >= fds.size()) {
    return absl::InternalError("Spawn response has an invalid pidfd index");
  }
  pidfd = std::move(fds[response.pidfd_index()]);
#endif
  // Track the process in the zygote (for notification pipe EOF) and in
  // StageZero (for the pid lookup of exit notifications).
  AddVirtualProcess(process);
  if (!stagezero_.AddVirtualProcess(response.pid(), process)) {
    return absl::InternalError(
        absl::StrFormat("Failed to add virtual process %s", process->Name()));
  }
  // The receiver can yield while reading this response, so the zygote may
  // already have reported the process's exit, or closed its notification
  // pipe, before it was registered.
  if (auto it = early_exits_.find(response.pid()); it != early_exits_.end()) {
    process->Notify(it->second);
    early_exits_.erase(it);
  } else if (notifications_closed_) {
    process->CloseNotifyPipe();
  }
  return std::make_pair(response.pid(), std::move(pidfd));
}

void Zygote::HandleExitNotification(int pid, int64_t status) {
  if (std::shared_ptr<Process> p = stagezero_.FindVirtualProcess(pid);
      p != nullptr) {
    p->Notify(status);
    return;
  }
  // The pid may belong to a spawn whose response is still being read.
  if (!pending_spawn_replies_.empty()) {
    early_exits_[pid] = status;
    return;
  }
  client_->Log(Name(), toolbelt::LogLevel::kError,
               "Can't find virtual process %d\n", pid);
}

void Zygote::HandleNotificationsClosed() {
  notifications_closed_ = true;
  ForeachVirtualProcess(
      [](std::shared_ptr<VirtualProcess> p) { p->CloseNotifyPipe(); });
}

Zygote::SpawnResult
Zygote::Spawn(std::shared_ptr<VirtualProcess> process,
              const stagezero::control::LaunchVirtualProcessRequest &req,
              const std::vector<std::shared_ptr<StreamInfo>> &streams,
              co::Coroutine *c) {
  if (!control_channel_open_) {
    return absl::InternalError(
        absl::StrFormat("Zygote %s: Socket is not connected", Name()));
  }
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
    case proto::StreamControl::PARAMETERS_READ:
      fds.push_back(stream->pipe.ReadFd());
      spawn.set_parameters_read_fd_index(fd_index++);
      break;
    case proto::StreamControl::PARAMETERS_WRITE:
      fds.push_back(stream->pipe.WriteFd());
      spawn.set_parameters_write_fd_index(fd_index++);
      break;
    case proto::StreamControl::PARAMETERS_EVENTS:
      fds.push_back(stream->pipe.ReadFd());
      spawn.set_parameters_events_fd_index(fd_index++);
      break;
    case proto::StreamControl::TELEMETRY_READ:
      fds.push_back(stream->pipe.ReadFd());
      spawn.set_telemetry_read_fd_index(fd_index++);
      break;
    case proto::StreamControl::TELEMETRY_WRITE:
      fds.push_back(stream->pipe.WriteFd());
      spawn.set_telemetry_write_fd_index(fd_index++);
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

  // Namespaces.
  if (req.opts().has_ns()) {
    auto *n = spawn.mutable_ns();
    *n = req.opts().ns();
  }

  // A virtual process that names no user takes the zygote's virtual process
  // user and group.  One that names a user but no group keeps that user's
  // primary group.
  if (req.opts().user().empty()) {
    spawn.set_user(req_.opts().virtual_process_user());
    spawn.set_group(req.opts().group().empty()
                        ? req_.opts().virtual_process_group()
                        : req.opts().group());
  } else {
    spawn.set_user(req.opts().user());
    spawn.set_group(req.opts().group());
  }
  spawn.set_cgroup(req.opts().cgroup());

  for (auto cpu : req.opts().cpus()) {
    spawn.add_cpus(cpu);
  }

  // kernel scheduler.
  if (req.opts().has_kernel_scheduler_policy()) {
    *spawn.mutable_kernel_scheduler_policy() =
        req.opts().kernel_scheduler_policy();
  }

  // Capabilities.
  *spawn.mutable_capabilities() = req.opts().capabilities();

  spawn.set_cgroup_root_dir(stagezero_.cgroup_root_dir_);

  std::vector<char> buffer(spawn.ByteSizeLong() + sizeof(int32_t));
  char *buf = buffer.data() + sizeof(int32_t);
  size_t buflen = buffer.size() - sizeof(int32_t);
  if (!spawn.SerializeToArray(buf, buflen)) {
    return absl::InternalError("Failed to serialize spawn message");
  }

  absl::StatusOr<toolbelt::TriggerFd> trigger = CreateCloseOnExecTriggerFd();
  if (!trigger.ok()) {
    return trigger.status();
  }
  auto reply =
      std::make_shared<SpawnReply>(std::move(*trigger), std::move(process));
  pending_spawn_replies_.push_back(reply);
  outgoing_spawns_.push_back(
      OutgoingSpawn{.buffer = std::move(buffer), .fds = std::move(fds)});
  outgoing_spawns_trigger_.Trigger();

  // The reply is completed exactly once: by the receiver or by
  // CloseControlChannel.
  int reply_fd = reply->trigger.GetPollFd().Fd();
  while (!reply->result.has_value()) {
    c->Wait(reply_fd, POLLIN);
  }
  return std::move(*reply->result);
}

VirtualProcess::VirtualProcess(
    co::CoroutineScheduler &scheduler, StageZero &stagezero,
    std::shared_ptr<ClientHandler> client,
    const stagezero::control::LaunchVirtualProcessRequest &&req)
    : Process(scheduler, stagezero, std::move(client), req.opts().name()),
      req_(std::move(req)) {
  for (auto &var : req.opts().vars()) {
    local_symbols_.AddSymbol(var.name(), var.value(), var.exported());
  }
  SetSignalTimeouts(req.opts().sigint_shutdown_timeout_secs(),
                    req.opts().sigterm_shutdown_timeout_secs(),
                    req.opts().telemetry_shutdown_timeout_secs());
  SetUserAndGroup(req.opts().user(), req.opts().group());
  SetCgroup(req.opts().cgroup());
  SetDetached(req.opts().detached());
  critical_ = req.opts().critical();
  if (req.opts().has_ns()) {
    Namespace n;
    n.FromProto(req.opts().ns());
    SetNamespace(std::move(n));
  }

  // Create the notification pipe for zygote notifications.
  absl::StatusOr<toolbelt::Pipe> pipe = toolbelt::Pipe::Create();
  if (!pipe.ok()) {
    abort();
  }
  notify_pipe_ = std::move(*pipe);
  for (auto cpu : req.opts().cpus()) {
    cpus_.push_back(cpu);
  }
  // kernel scheduler.
  if (req.opts().has_kernel_scheduler_policy()) {
    kernel_scheduler_policy_.FromProto(req.opts().kernel_scheduler_policy());
  }
  // Capabilities.
  capabilities_.FromProto(req_.opts().capabilities());
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
  if (absl::Status status = BuildStreams(req_.streams(), req_.opts().notify(),
                                         req_.opts().telemetry());
      !status.ok()) {
    return status;
  }

#if defined(__linux__)
  if (absl::Status status = BuildLinuxParameters(); !status.ok()) {
    return status;
  }
#endif

  Zygote::SpawnResult pids = zygote_->Spawn(vproc, req_, GetStreams(), c);
  if (!pids.ok()) {
    return absl::InternalError(
        absl::StrFormat("Failed to spawn virtual process %s: %s", Name(),
                        pids.status().ToString()));
  }
  SetPid(pids->first);
  SetProcessId();
#if defined(__linux__) && HAVE_PIDFD
  SetPidFd(std::move(pids->second));
#endif

  // Run a coroutine to process commands from the parameter stream.  This is
  // started before the process is ready so it can use parameters during its
  // startup.
  RunParameterServer();
  if (UseTelemetry()) {
    RunTelemetryServer();
  }

  // As for a static process, the launch completes once the process exists.
  // Readiness is reported as a start event and any exit, including one before
  // readiness, as a stop event.
  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_,
      [proc, vproc, zygote = zygote_, client = client_](co::Coroutine *c2) {
        absl::Status eventStatus;
        if (vproc->WaitForReadiness(c2)) {
          // Send start event to client.
          if (proc->IsDetached()) {
            eventStatus =
                proc->GetStageZero().SendProcessStartEvent(proc->GetId());
          } else {
            eventStatus = client->SendProcessStartEvent(proc->GetId());
          }
          if (!eventStatus.ok()) {
            client->Log(proc->Name(), toolbelt::LogLevel::kError, "%s",
                        eventStatus.ToString().c_str());
          }
        }

        int status = vproc->WaitForZygoteNotification(c2);
        zygote->RemoveVirtualProcess(vproc);
        // As for a static process, it may have exited without being stopped,
        // in which case it is still in the maps.
        client->TryRemoveProcess(proc);

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
        if (proc->IsDetached()) {
          eventStatus = proc->GetStageZero().SendProcessStopEvent(
              proc->GetId(), !signaled, exit_status, term_sig);
        } else {
          eventStatus = client->SendProcessStopEvent(proc->GetId(), !signaled,
                                                     exit_status, term_sig);
        }
        if (!eventStatus.ok()) {
          client->Log(proc->Name(), toolbelt::LogLevel::kError, "%s\n",
                      eventStatus.ToString().c_str());
          return;
        }
      }));
  return absl::OkStatus();
}

#if defined(__linux__)
absl::Status Process::BuildLinuxParameters() {
  // Build a local parameter for the policy and cpu affinity.  The process
  // can read this to set the policy and cpu affinity for any threads it
  // needs to.  This is up to the process as to how it will manage its threads.
  stagezero::config::ProcessKernelPolicyAndCpuAffinity policy_and_affinity;
  auto p = policy_and_affinity.mutable_policy();
  kernel_scheduler_policy_.ToProto(p);
  for (int cpu : cpus_) {
    policy_and_affinity.add_cpus(cpu);
  }
  std::string encoded = policy_and_affinity.SerializeAsString();

  if (auto status = local_parameters_.SetParameter(
          "kernel_policy_and_cpu_affinity", encoded);
      !status.ok()) {
    return status;
  }

  // Build a local parameter for the capabilities.
  stagezero::config::CapabilitySet caps;
  capabilities_.ToProto(&caps);

  encoded = caps.SerializeAsString();
  if (auto status = local_parameters_.SetParameter("capabilities", encoded);
      !status.ok()) {
    return status;
  }
  return absl::OkStatus();
}
#endif

int VirtualProcess::Wait() {
  int e = SafeKill(pid_, 0);
  if (e == -1) {
    running_ = false;
    return 127;
  }
  return 0;
}

bool VirtualProcess::WaitForReadiness(co::Coroutine *c) {
  if (!WillNotify()) {
    return true;
  }
  std::shared_ptr<StreamInfo> s = FindNotifyStream();
  if (s == nullptr) {
    return true;
  }
  int notify_fd = s->pipe.ReadFd().Fd();
  int exit_fd = notify_pipe_.ReadFd().Fd();
  uint64_t timeout_ns = StartupTimeoutSecs() * 1000000000LL;
  int wait_fd =
      c->Wait(std::vector<int>{notify_fd, exit_fd}, POLLIN, timeout_ns);
  if (wait_fd == -1) {
    client_->Log(Name(), toolbelt::LogLevel::kError,
                 "Process %s failed to notify us of startup after %d seconds",
                 Name().c_str(), StartupTimeoutSecs());
    // Stop the process as it failed to notify us.
    (void)Stop(c);
    return false;
  }
  if (wait_fd == exit_fd) {
    // The process exited (or the zygote did) before becoming ready.  The exit
    // status is left in the pipe for WaitForZygoteNotification.
    return false;
  }
  int64_t val;
  ssize_t n = read(notify_fd, &val, sizeof(val));
  if (n != sizeof(val)) {
    // As for a static process, it keeps running without a start event and
    // the client can stop it.
    client_->Log(Name(), toolbelt::LogLevel::kError,
                 "Failed to read startup notification from process %s: %s",
                 Name().c_str(), n == -1 ? strerror(errno) : "unexpected EOF");
    return false;
  }
  client_->Log(Name(), toolbelt::LogLevel::kDebug,
               "Process %s notified us of startup", Name().c_str());
  return true;
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
  return stagezero::AddToCgroup(Name(), cgroup_, pid, stagezero_.cgroup_root_dir_, client_->GetLogger());
}
} // namespace adastra::stagezero
