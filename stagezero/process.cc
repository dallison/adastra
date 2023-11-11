#include "stagezero/process.h"

#include "absl/strings/str_format.h"
#include "stagezero/client_handler.h"
#include "toolbelt/hexdump.h"

#include <ctype.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <limits.h>
#include <sys/wait.h>
#include <vector>
#if defined(_linux__)
#include <pty.h
#elif defined(__APPLE__)
#include <util.h>
#else
#error "Unknown OS"
#endif

namespace stagezero {

Process::Process(co::CoroutineScheduler &scheduler,
                 std::shared_ptr<ClientHandler> client, std::string name)
    : scheduler_(scheduler), client_(std::move(client)), name_(std::move(name)),
      local_symbols_(client_->GetGlobalSymbols()) {}

void Process::SetProcessId() {
  process_id_ =
      absl::StrFormat("%s/%s:%d", client_->GetClientName(), name_, pid_);
}

void Process::KillNow() {
  if (pid_ <= 0) {
    return;
  }
  ::kill(-pid_, SIGKILL);
  (void)client_->RemoveProcess(this);
}

int Process::WaitLoop(co::Coroutine *c, int timeout_secs) {
  constexpr int kWaitTimeMs = 100;
  int num_iterations = timeout_secs * 1000 / kWaitTimeMs;
  int status = 0;
  for (;;) {
    status = Wait();
    if (!running_) {
      break;
    }
    c->Millisleep(kWaitTimeMs);
    if (timeout_secs != -1) {
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
    if (stream->disposition == control::StreamControl::NOTIFY) {
      return stream;
    }
  }
  return nullptr;
}

StaticProcess::StaticProcess(
    co::CoroutineScheduler &scheduler, std::shared_ptr<ClientHandler> client,
    const stagezero::control::LaunchStaticProcessRequest &&req)
    : Process(scheduler, std::move(client), req.opts().name()), req_(std::move(req)) {
  for (auto &var : req.opts().vars()) {
    local_symbols_.AddSymbol(var.name(), var.value(), var.exported());
  }
  SetSignalTimeouts(req.opts().sigint_shutdown_timeout_secs(),
                    req.opts().sigterm_shutdown_timeout_secs());
}

absl::Status StaticProcess::Start(co::Coroutine *c) {
  return StartInternal({}, true);
}

absl::Status
StaticProcess::StartInternal(const std::vector<std::string> extra_env_vars,
                             bool send_start_event) {
  client_->GetLogger().Log(toolbelt::LogLevel::kInfo, "starting %s",
                           req_.proc().executable().c_str());
  absl::Status s = ForkAndExec(extra_env_vars);
  if (!s.ok()) {
    return s;
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
            int notify_fd = s->read_fd.Fd();
            std::cerr << "waiting for notify on fd " << notify_fd << std::endl;
            uint64_t timeout_ns = proc->StartupTimeoutSecs() * 1000000000LL;
            int wait_fd = c->Wait(notify_fd, POLLIN, timeout_ns);
            if (wait_fd == -1) {
              // Timeout waiting for notification.
              client->GetLogger().Log(
                  toolbelt::LogLevel::kError,
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

            client->GetLogger().Log(toolbelt::LogLevel::kInfo,
                                    "Process %s notified us of startup",
                                    proc->Name().c_str());
          }
        }
        // Send start event to client.
        if (send_start_event) {
          absl::Status eventStatus =
              client->SendProcessStartEvent(proc->GetId());
          if (!eventStatus.ok()) {
            client->GetLogger().Log(toolbelt::LogLevel::kError, "%s\n",
                                    eventStatus.ToString().c_str());
            return;
          }
        }
        int status = proc->WaitLoop(c, -1);
        std::cerr << "process " << proc->Name() << " exited with status "
                  << status << std::endl;

        // The process might have died due to an external signal.  If we didn't
        // kill it, we won't have removed it from the maps.  We try to do this
        // now but ignore it if it's already gone.
        client->TryRemoveProcess(proc);

        absl::Status eventStatus;
        if (proc->IsStopping()) {
          // Intentionally stopped.
          eventStatus = client->SendProcessStopEvent(proc->GetId(), true, 0, 0);
        } else {
          // Unintentional stop.
          eventStatus = client->SendProcessStopEvent(proc->GetId(), true, 0, 0);
        }
        if (!eventStatus.ok()) {
          client->GetLogger().Log(toolbelt::LogLevel::kError, "%s\n",
                                  eventStatus.ToString().c_str());
          return;
        }
      },
      name_.c_str()));
  return absl::OkStatus();
}

// Returns a pair of open file descriptors.  The first is the stagezero end
// and the second is the process end.
static absl::StatusOr<std::pair<int, int>> MakeFileDescriptors(bool istty) {
  if (istty) {
    std::cerr << "opening pty for process" << std::endl;
    int this_end, proc_end;
    // TODO: terminal parameters.
    int e = openpty(&this_end, &proc_end, nullptr, nullptr, nullptr);
    if (e == -1) {
      return absl::InternalError(absl::StrFormat(
          "Failed to open pty for stream: %s", strerror(errno)));
    }
    return std::make_pair(this_end, proc_end);
  }

  // Use pipe.
  int pipes[2];
  int e = pipe(pipes);
  if (e == -1) {
    return absl::InternalError(
        absl::StrFormat("Failed to open pipe for stream: %s", strerror(errno)));
  }
  std::cerr << "fds: read: " << pipes[0] << " write: " << pipes[1] << std::endl;
  return std::make_pair(pipes[0], pipes[1]);
}

absl::Status StreamFromFileDescriptor(
    int fd, std::function<absl::Status(const char *, size_t)> writer,
    co::Coroutine *c) {
  char buffer[256];
  for (;;) {
    std::cerr << "waiting for fd " << fd << std::endl;
    int wait_fd = c->Wait(fd, POLLIN);
    if (wait_fd != fd) {
      std::cerr << "wait returned " << wait_fd << std::endl;
      return absl::InternalError("Interrupted");
    }
    std::cerr << "reading from " << fd << std::endl;
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
  std::cerr << "Writing to fd " << fd << std::endl;
  while (remaining > 0) {
    std::cerr << "waiting" << std::endl;
    int wait_fd = c->Wait(fd, POLLOUT);
    if (wait_fd != fd) {
      return absl::InternalError("Interrupted");
    }
    std::cerr << "writing" << std::endl;
    ssize_t n = ::write(fd, buf, remaining);
    std::cerr << "result " << n << std::endl;
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

absl::Status Process::BuildStreams(
    const google::protobuf::RepeatedPtrField<control::StreamControl> &streams,
    bool notify) {
  // If the process is going to notify us of startup, make a pipe
  // for it to use and build a StreamInfo for it.
  if (notify) {
    auto stream = std::make_shared<StreamInfo>();
    stream->disposition = control::StreamControl::NOTIFY;
    stream->direction = control::StreamControl::OUTPUT;
    absl::StatusOr<std::pair<int, int>> fds = MakeFileDescriptors(false);
    if (!fds.ok()) {
      return fds.status();
    }
    stream->read_fd.SetFd(fds->first);
    stream->write_fd.SetFd(fds->second);
    streams_.push_back(stream);
    std::cerr << "notify fds are " << fds->first << "/" << fds->second
              << std::endl;
  }

  for (const control::StreamControl &s : streams) {
    auto stream = std::make_shared<StreamInfo>();
    control::StreamControl::Direction direction = s.direction();
    stream->direction = direction;
    stream->fd = s.stream_fd();
    stream->disposition = s.disposition();

    streams_.push_back(stream);

    switch (stream->disposition) {
    case control::StreamControl::CLOSE:
      break;
    case control::StreamControl::CLIENT: {
      absl::StatusOr<std::pair<int, int>> fds = MakeFileDescriptors(s.tty());
      if (!fds.ok()) {
        return fds.status();
      }
      stream->read_fd.SetFd(fds->first);
      stream->write_fd.SetFd(fds->second);

      // For an output stream start a coroutine to read from the pipe/tty
      // and send as an event.
      //
      // An input stream is handled by an incoming InputData command that writes
      // to the write end of the pipe/tty.
      if (stream->direction == control::StreamControl::OUTPUT) {
        client_->AddCoroutine(std::make_unique<co::Coroutine>(
            scheduler_, [ proc = shared_from_this(), stream,
                          client = client_ ](co::Coroutine * c) {
              std::cout << "relay to client coroutine running" << std::endl;
              absl::Status status = StreamFromFileDescriptor(
                  stream->read_fd.Fd(),
                  [proc, stream, client](const char *buf,
                                         size_t len) -> absl::Status {
                    // Write to client using an event.
                    return client->SendOutputEvent(proc->GetId(), stream->fd,
                                                   buf, len);
                  },
                  c);
              std::cout << "relay to client coroutine done" << std::endl;
            }));
      }
      break;
    }

    case control::StreamControl::FILENAME: {
      int oflag = stream->direction == control::StreamControl::INPUT
                      ? O_RDONLY
                      : (O_WRONLY | O_TRUNC | O_CREAT);

      int file_fd = open(local_symbols_.ReplaceSymbols(s.filename()).c_str(),
                         oflag, 0777);
      if (file_fd == -1) {
        return absl::InternalError(
            absl::StrFormat("Failed to open file %s", strerror(errno)));
      }
      // Set the process end of the stream (the fd that will be redirected)
      // to the file's open fd.
      if (stream->direction == control::StreamControl::OUTPUT) {
        stream->write_fd.SetFd(file_fd);
      } else {
        stream->read_fd.SetFd(file_fd);
      }
      break;
    }
    case control::StreamControl::FD:
      // Set the process end of the stream (the fd that will be redirected)
      // to the fd specified by the user.
      if (stream->direction == control::StreamControl::OUTPUT) {
        stream->write_fd.SetFd(s.fd());
      } else {
        stream->read_fd.SetFd(s.fd());
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
  pid_ = fork();
  if (pid_ == -1) {
    return absl::InternalError(
        absl::StrFormat("Fork failed: %s", strerror(errno)));
  }
  if (pid_ == 0) {
    // Stop the coroutine scheduler in this process.  We have forked so the
    // all the coroutines in the parent process are also in this child process.
    // We don't want them to run in the child, so we stop the scheduler.
    client_->GetScheduler().Stop();

    // Redirect the streams.
    for (auto &stream : streams_) {
      if (stream->disposition != control::StreamControl::CLOSE) {
        // For a notify we don't redirect an fd, but instead tell
        // the process what it is via an environment variable.
        // For other streams, we redirect to the given file descriptor
        // number and close the duplicated file descriptor.
        if (stream->disposition != control::StreamControl::NOTIFY) {
          toolbelt::FileDescriptor &fd =
              stream->direction == control::StreamControl::OUTPUT
                  ? stream->write_fd
                  : stream->read_fd;
          std::cerr << "redirecting fd " << fd.Fd() << " to " << stream->fd
                    << std::endl;
          (void)close(stream->fd);
          int e = dup2(fd.Fd(), stream->fd);
          if (e == -1) {
            std::cerr << "Failed to redirect file descriptor" << std::endl;
            exit(1);
          }

          // Close the duplicated fd.
          fd.Reset();
        }

        if (stream->disposition == control::StreamControl::CLIENT ||
            stream->disposition == control::StreamControl::NOTIFY) {
          // Close the duplicated other end of the pipes.
          toolbelt::FileDescriptor &fd =
              stream->direction == control::StreamControl::OUTPUT
                  ? stream->read_fd
                  : stream->write_fd;
          fd.Reset();
        }
      }
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
    argv.push_back(exe.c_str());
    for (auto &arg : args) {
      argv.push_back(arg.c_str());
    }
    argv.push_back(nullptr);

    // Build the environment strings.
    std::vector<std::string> env_strings;
    if (req_.opts().notify()) {
      // For a notify fd, set the STAGEZERO_NOTIFY_FD environment
      // variable.  The process will write a single arbitrary byte to this
      // to tell us that it has started.
      std::shared_ptr<StreamInfo> notify_stream = FindNotifyStream();
      if (notify_stream != nullptr) {
        env_strings.push_back(absl::StrFormat("STAGEZERO_NOTIFY_FD=%d",
                                              notify_stream->write_fd.Fd()));
      }
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

    execve(exe.c_str(),
           reinterpret_cast<char *const *>(const_cast<char **>(argv.data())),
           reinterpret_cast<char *const *>(const_cast<char **>(env.data())));
    std::cerr << "Failed to exec " << argv[0] << ": " << strerror(errno)
              << std::endl;
    exit(1);
  }

  // Close redirected stream fds in parent.
  for (auto &stream : streams_) {
    if (stream->disposition != control::StreamControl::CLOSE) {
      toolbelt::FileDescriptor &fd =
          stream->direction == control::StreamControl::OUTPUT ? stream->write_fd
                                                              : stream->read_fd;
      printf("ForkAndExec: closing %d\n", fd.Fd());
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
          client->GetLogger().Log(
              toolbelt::LogLevel::kInfo,
              "Killing process %s with SIGINT (timeout %d seconds)",
              proc->Name().c_str(), timeout);
          kill(-proc->GetPid(), SIGINT);
          (void)proc->WaitLoop(c2, timeout);
          if (!proc->IsRunning()) {
            return;
          }
        }
        timeout = proc->SigTermTimeoutSecs();
        if (timeout > 0) {
          kill(-proc->GetPid(), SIGTERM);
          client->GetLogger().Log(
              toolbelt::LogLevel::kInfo,
              "Killing process %s with SIGTERM (timeout %d seconds)",
              proc->Name().c_str(), timeout);
          (void)proc->WaitLoop(c2, timeout);
        }

        // Always send SIGKILL if it's still running.  It can't ignore this.
        if (proc->IsRunning()) {
          client->GetLogger().Log(toolbelt::LogLevel::kInfo,
                                  "Killing process %s with SIGKILL",
                                  proc->Name().c_str());

          kill(-proc->GetPid(), SIGKILL);
        }
      }));
  return client_->RemoveProcess(this);
}

absl::Status Process::SendInput(int fd, const std::string &data,
                                co::Coroutine *c) {
  for (auto &stream : streams_) {
    if (stream->fd == fd) {
      return WriteToProcess(stream->write_fd.Fd(), data.data(), data.size(), c);
    }
  }
  return absl::InternalError(absl::StrFormat("Unknown stream fd %d", fd));
}

absl::Status Process::CloseFileDescriptor(int fd) {
  std::cerr << "closing " << fd << std::endl;
  for (auto &stream : streams_) {
    if (stream->fd == fd) {
      std::cerr << "found, close" << std::endl;
      int stream_fd = stream->direction == control::StreamControl::INPUT
                          ? stream->write_fd.Fd()
                          : stream->read_fd.Fd();
      std::cerr << "close " << stream_fd << std::endl;
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

  std::pair<std::string, int> socket_and_fd = BuildControlSocketName();
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

  std::cerr << "Starting zygote static process " << socket_name << std::endl;

  if (absl::Status status =
          StartInternal(zygoteEnv, /*send_start_event=*/false);
      !status.ok()) {
    return status;
  }

  // Wait for the zygote to connect to the socket in a coroutine.
  client_->AddCoroutine(std::make_unique<co::Coroutine>(scheduler_, [
    proc = shared_from_this(), listen_socket = std::move(listen_socket),
    client = client_
  ](co::Coroutine * c) mutable {
    std::cerr << "zygote acceptor running " << std::endl;
    absl::StatusOr<toolbelt::UnixSocket> s = listen_socket.Accept(c);
    if (!s.ok()) {
      client->GetLogger().Log(toolbelt::LogLevel::kError,
                              "Unable to accept connection from zygote %s: %s",
                              proc->Name().c_str(),
                              s.status().ToString().c_str());
      return;
    }
    auto *zygote = static_cast<Zygote *>(proc.get());
    zygote->SetControlSocket(std::move(*s));
    client->GetLogger().Log(toolbelt::LogLevel::kInfo,
                            "Zygote control socket open");
    absl::Status eventStatus = client->SendProcessStartEvent(proc->GetId());
    if (!eventStatus.ok()) {
      client->GetLogger().Log(toolbelt::LogLevel::kError, "%s\n",
                              eventStatus.ToString().c_str());
      return;
    }
  }));
  return absl::OkStatus();
}

std::pair<std::string, int> Zygote::BuildControlSocketName() {
  char socket_file[NAME_MAX]; // Unique file in file system.
  snprintf(socket_file, sizeof(socket_file), "/tmp/zygote-%s.XXXXXX",
           Name().c_str());
  int tmpfd = mkstemp(socket_file);
  return std::make_pair(socket_file, tmpfd);
}

absl::StatusOr<int>
Zygote::Spawn(const stagezero::control::LaunchVirtualProcessRequest &req,
              const std::vector<std::shared_ptr<StreamInfo>> &streams,
              co::Coroutine *c) {
  std::cerr << "Zygote spawning" << std::endl;
  control::SpawnRequest spawn;
  spawn.set_dso(req.proc().dso());
  spawn.set_main_func(req.proc().main_func());

  // Streams.
  std::vector<toolbelt::FileDescriptor> fds;
  int index = 0;
  for (auto &stream : streams) {
    auto *s = spawn.add_streams();
    if (stream->disposition != control::StreamControl::CLOSE) {
      const toolbelt::FileDescriptor &fd =
          stream->direction == control::StreamControl::OUTPUT ? stream->write_fd
                                                              : stream->read_fd;
      s->set_fd(stream->fd);
      fds.push_back(fd);
      s->set_index(index++);
    } else {
      s->set_close(true);
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

  std::vector<char> buffer(spawn.ByteSizeLong() + sizeof(int32_t));
  char *buf = buffer.data() + sizeof(int32_t);
  size_t buflen = buffer.size() - sizeof(int32_t);
  if (!spawn.SerializeToArray(buf, buflen)) {
    return absl::InternalError("Failed to serilize spawn message");
  }

  absl::StatusOr<ssize_t> n = control_socket_.SendMessage(buf, buflen, c);
  if (!n.ok()) {
    control_socket_.Close();
    return n.status();
  }

  if (absl::Status s = control_socket_.SendFds(fds, c); !s.ok()) {
    control_socket_.Close();
    return s;
  }

  // Wait for response and put it in the same buffer we used for send.
  n = control_socket_.ReceiveMessage(buffer.data(), buffer.size(), c);
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
    : Process(scheduler, std::move(client), req.opts().name()), req_(std::move(req)) {
  for (auto &var : req.opts().vars()) {
    local_symbols_.AddSymbol(var.name(), var.value(), var.exported());
  }
  SetSignalTimeouts(req.opts().sigint_shutdown_timeout_secs(),
                    req.opts().sigterm_shutdown_timeout_secs());
}

absl::Status VirtualProcess::Start(co::Coroutine *c) {
  std::shared_ptr<Zygote> zygote = client_->FindZygote(req_.proc().zygote());
  if (zygote == nullptr) {
    return absl::InternalError(
        absl::StrFormat("No such zygote %s", req_.proc().zygote()));
  }

  if (absl::Status status = BuildStreams(req_.streams(), req_.opts().notify());
      !status.ok()) {
    return status;
  }
  absl::StatusOr<int> pid = zygote->Spawn(req_, GetStreams(), c);
  if (!pid.ok()) {
    return absl::InternalError(
        absl::StrFormat("Failed to spawn virtual process %s: %s", Name(),
                        pid.status().ToString()));
  }
  SetPid(*pid);
  SetProcessId();

  client_->AddCoroutine(std::make_unique<co::Coroutine>(
      scheduler_, [ proc = shared_from_this(), zygote,
                    client = client_ ](co::Coroutine * c2) {
        // Send start event to client->
        absl::Status eventStatus = client->SendProcessStartEvent(proc->GetId());
        if (!eventStatus.ok()) {
          client->GetLogger().Log(toolbelt::LogLevel::kError, "%s",
                                  eventStatus.ToString().c_str());
          return;
        }

        printf("virtual process waiting\n");

        int status = proc->WaitLoop(c2, -1);
        std::cerr << "virtual process status " << status << std::endl;
        if (proc->IsStopping()) {
          // Intentionally stopped.
          eventStatus = client->SendProcessStopEvent(proc->GetId(), true, 0, 0);
        } else {
          // Unintentional stop.
          eventStatus = client->SendProcessStopEvent(proc->GetId(), true, 0, 0);
        }
        if (!eventStatus.ok()) {
          client->GetLogger().Log(toolbelt::LogLevel::kError, "%s\n",
                                  eventStatus.ToString().c_str());
          return;
        }
      }));

  return absl::OkStatus();
}

int VirtualProcess::Wait() {
  int e = ::kill(pid_, 0);
  if (e == -1) {
    running_ = false;
    printf("virtual process terminated\n");
    return 127;
  }
  return 0;
}

} // namespace stagezero
