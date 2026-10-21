// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "stagezero/zygote/zygote_core.h"
#include "common/namespace.h"
#include "common/capability.h"
#include "stagezero/cgroup.h"

#include "absl/container/flat_hash_set.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "toolbelt/fd.h"
#include "toolbelt/hexdump.h"

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include <dirent.h>
#include <dlfcn.h>
#include <iostream>
#include <stdlib.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <grp.h>
#include <pwd.h>

#ifdef __linux__
#include <linux/sched.h>
#include <syscall.h>
#include <linux/capability.h>
#include <linux/securebits.h>
#include <sched.h>
#include <sys/prctl.h>

static int pidfd_open(pid_t pid, unsigned int flags) {
  return syscall(__NR_pidfd_open, pid, flags);
}

#endif

extern "C" char **environ;

ABSL_FLAG(std::string, run, "", "Run this module directly");
ABSL_FLAG(std::string, main, "ModuleMain", "Name of main function");
ABSL_FLAG(std::vector<std::string>, vars, {}, "Variables (name=value) pairs");

namespace adastra::stagezero {

// Closes every open file descriptor not in keep.  Only the open fds are
// visited: RLIMIT_NOFILE is often over a million, and probing every possible
// fd delays the start of each virtual process by seconds under load.
static void CloseFdsExcept(const absl::flat_hash_set<int> &keep) {
  if (DIR *dir = opendir("/proc/self/fd"); dir != nullptr) {
    int dir_fd = dirfd(dir);
    std::vector<int> to_close;
    while (struct dirent *entry = readdir(dir)) {
      int fd;
      if (absl::SimpleAtoi(entry->d_name, &fd) && fd != dir_fd &&
          !keep.contains(fd)) {
        to_close.push_back(fd);
      }
    }
    closedir(dir);
    for (int fd : to_close) {
      (void)close(fd);
    }
    return;
  }
  toolbelt::CloseAllFds([&keep](int fd) { return !keep.contains(fd); });
}

class ZygoteSystemTelemetry : public ::stagezero::SystemTelemetry {
public:
  ZygoteSystemTelemetry(::stagezero::Telemetry &telemetry)
      : SystemTelemetry(telemetry, "ZygoteCore::SystemTelemetry") {}

  absl::Status
  HandleCommand(std::unique_ptr<::stagezero::TelemetryCommand> command,
                co::Coroutine *c) override {

    switch (command->Code()) {
    case ::stagezero::SystemTelemetry::kShutdownCommand: {
      auto shutdown =
          static_cast<::stagezero::ShutdownCommand *>(command.get());
      std::cerr << "Zygote is shutting down due to telemetry command\n";
      exit(shutdown->exit_code);
      break;
    }

    default:
      break;
    }
    return absl::OkStatus();
  }
};

ZygoteCore::ZygoteCore(int argc, char **argv)
    : logger_("zygote_core"), telemetry_(scheduler_) {
  absl::ParseCommandLine(argc, argv);

  for (int i = 0; i < argc; i++) {
    args_.push_back(argv[i]);
  }
}

absl::Status ZygoteCore::Run() {
  char *notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd;
    if (!absl::SimpleAtoi(notify, &notify_fd)) {
      return absl::InternalError(
          absl::StrFormat("Invalid STAGEZERO_NOTIFY_FD value: %s", notify));
    }
    int64_t val = 1;
    (void)write(notify_fd, &val, 8);
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

  server_ = std::make_unique<co::Coroutine>(
      scheduler_,
      [this](co::Coroutine *c) {
        while (control_socket_->Connected()) {
          WaitForSpawn(c);
        }
      },
      "ZygoteServer");

  if (telemetry_.IsOpen()) {
    auto zt = std::make_shared<ZygoteSystemTelemetry>(telemetry_);
    telemetry_.AddModule(std::move(zt));

    // Run the telemetry module as a coroutine in our scheduler.
    telemetry_.Run();
  }

  monitor_ = std::make_unique<co::Coroutine>(
      scheduler_,
      [this](co::Coroutine *c) {
        constexpr int kWaitTimeMs = 500;

        for (;;) {
          int status;
          int wait_arg = WNOHANG;
#if defined(__linux__)
          wait_arg |= __WALL;
#endif
          pid_t pid = waitpid(-1, &status, wait_arg);
          if (pid > 0) {
            // A process died.  Write the pid and status to the notification
            // pipe.  Set the top bit to distinguish this from any other
            // notification.
            logger_.Log(toolbelt::LogLevel::kDebug, "Process %d died", pid);
            uint64_t val = (1LL << 63LL) |
                           (static_cast<uint64_t>(pid) << 32LL) |
                           static_cast<uint64_t>(status);
            (void)write(notification_pipe_.Fd(), &val, 8);
          }
          c->Millisleep(kWaitTimeMs);
        }
      },
      "ZygoteMonitor");

  scheduler_.Run();
  // When we get here we are either in the zygote that has been stopped or
  // we are in the forked process and will invoke the main function.

  if (forked_) {
    // We want to destruct the ZygoteCore as we don't need it in the
    // child process.
    AfterFork a = std::move(after_fork_);
    std::string exe = args_[0];
    ZygoteCore::~ZygoteCore();
    a.local_symbols->ClearParent();
    InvokeMainAfterSpawn(std::move(exe), std::move(a.req),
                         std::move(a.local_symbols));
    // Will never get here.
  }
  return absl::OkStatus();
}

void ZygoteCore::WaitForSpawn(co::Coroutine *c) {
  char *sendbuf = buffer_ + sizeof(int32_t);
  constexpr size_t kSendBufLen = sizeof(buffer_) - sizeof(int32_t);

  int fd = c->Wait(control_socket_->GetFileDescriptor().Fd());
  if (fd != control_socket_->GetFileDescriptor().Fd()) {
    return;
  }

  absl::StatusOr<ssize_t> n =
      control_socket_->ReceiveMessage(buffer_, sizeof(buffer_));
  if (!n.ok()) {
    logger_.Log(toolbelt::LogLevel::kError, "ReceiveMessage error %s\n",
                n.status().ToString().c_str());
    control_socket_->Close();
    return;
  }

  std::vector<toolbelt::FileDescriptor> fds;

  if (absl::Status status = control_socket_->ReceiveFds(fds); !status.ok()) {
    logger_.Log(toolbelt::LogLevel::kError, "Failed to receive fds %s\n",
                status.ToString().c_str());
    control_socket_->Close();
    return;
  }

  stagezero::control::SpawnRequest request;
  if (request.ParseFromArray(buffer_, *n)) {
    stagezero::control::SpawnResponse response;

    if (absl::Status status = HandleSpawn(request, &response, fds, c);
        !status.ok()) {
      response.set_error(status.ToString());
    }
    if (!response.SerializeToArray(sendbuf, kSendBufLen)) {
      logger_.Log(toolbelt::LogLevel::kError, "Failed to serialize response");
      return;
    }
    size_t msglen = response.ByteSizeLong();
    absl::StatusOr<ssize_t> n = control_socket_->SendMessage(sendbuf, msglen);
    if (!n.ok()) {
      logger_.Log(toolbelt::LogLevel::kError,
                  "Failed to send spawn response: %s",
                  n.status().ToString().c_str());
      control_socket_->Close();
      return;
    }
    if (absl::Status s = control_socket_->SendFds(fds); !s.ok()) {
      logger_.Log(toolbelt::LogLevel::kError,
                  "Failed to send spawn fds response: %s",
                  s.ToString().c_str());
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
  auto symbols = std::make_unique<SymbolTable>(&global_symbols_);
  for (auto &var : vars) {
    std::string name;
    std::string value;
    size_t equal = var.find('=');
    if (equal != std::string::npos) {
      name = var.substr(0, equal);
      value = var.substr(equal + 1);
      symbols->AddSymbol(name, value, false);
    } else {
      symbols->AddSymbol(name, "", false);
    }
  }
  InvokeMainAfterSpawn(std::move(args_[0]), std::move(spawn),
                       std::move(symbols));
}

absl::Status ZygoteCore::SetUpChildAsRoot(const control::SpawnRequest &req) {
#if defined(__linux__)
  // Join the cgroup first: its cpuset may be what permits the requested CPUs,
  // and the target user can't write cgroup.procs.
  if (!req.cgroup().empty()) {
    if (absl::Status status =
            stagezero::AddToCgroup(req.name(), req.cgroup(), getpid(),
                                   req.cgroup_root_dir(), logger_);
        !status.ok()) {
      return status;
    }
  }

  if (req.kernel_scheduler_policy().policy() !=
      stagezero::config::KernelSchedulerPolicy::KERN_SCHED_DEFAULT) {
    int policy = 0;

    switch (req.kernel_scheduler_policy().policy()) {
    case stagezero::config::KernelSchedulerPolicy::KERN_SCHED_DEFAULT:
    default:
      break;
    case stagezero::config::KernelSchedulerPolicy::KERN_SCHED_BATCH:
      policy = SCHED_BATCH;
      break;
    case stagezero::config::KernelSchedulerPolicy::KERN_SCHED_IDLE:
      policy = SCHED_IDLE;
      break;
    case stagezero::config::KernelSchedulerPolicy::KERN_SCHED_FIFO:
      policy = SCHED_FIFO;
      break;
    case stagezero::config::KernelSchedulerPolicy::KERN_SCHED_RR:
      policy = SCHED_RR;
      break;
    }
    if (req.kernel_scheduler_policy().reset_on_fork()) {
      policy |= SCHED_RESET_ON_FORK;
    }
    struct sched_param param = {0};
    param.sched_priority = req.kernel_scheduler_policy().priority();
    int e = sched_setscheduler(0, policy, &param);
    if (e == -1) {
      return absl::InternalError(absl::StrFormat(
          "Failed to set scheduler policy: %s", strerror(errno)));
    }
  }

  if (!req.cpus().empty()) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    for (int cpu : req.cpus()) {
      CPU_SET(cpu, &cpuset);
    }
    int e = sched_setaffinity(0, sizeof(cpuset), &cpuset);
    if (e == -1) {
      return absl::InternalError(
          absl::StrFormat("Failed to set CPU affinity: %s", strerror(errno)));
    }
  }

  // Work out the ids to switch to.  A user brings its primary and
  // supplementary groups; a group overrides the primary group.
  uid_t uid = 0;
  gid_t gid = 0;
  std::vector<gid_t> groups;
  if (!req.user().empty()) {
    struct passwd *pw = getpwnam(req.user().c_str());
    if (pw == nullptr) {
      return absl::InternalError(
          absl::StrFormat("Unknown user %s", req.user()));
    }
    uid = pw->pw_uid;
    gid = pw->pw_gid;
    int ngroups = 0;
    getgrouplist(req.user().c_str(), gid, nullptr, &ngroups);
    if (ngroups > 0) {
      groups.resize(ngroups);
      if (getgrouplist(req.user().c_str(), gid, groups.data(), &ngroups) ==
          -1) {
        return absl::InternalError(absl::StrFormat(
            "Failed to get the groups of user %s", req.user()));
      }
      groups.resize(ngroups);
    }
  }
  if (!req.group().empty()) {
    struct group *gr = getgrnam(req.group().c_str());
    if (gr == nullptr) {
      return absl::InternalError(
          absl::StrFormat("Unknown group %s", req.group()));
    }
    gid = gr->gr_gid;
  }

  // Capabilities are kept across the change of user only if the process asks
  // for some and won't run as root.  It needs CAP_SETUID and CAP_SETGID to
  // change ids, which are dropped afterwards unless it asked for them.
  bool drop_uid_cap = false;
  bool drop_gid_cap = false;
  adastra::Caps current_caps;
  if (uid != 0 && !req.capabilities().capabilities().empty()) {
    int e = prctl(PR_SET_SECUREBITS, SECBIT_NO_SETUID_FIXUP, 0, 0, 0);
    if (e == -1) {
      return absl::InternalError(
          absl::StrFormat("Failed to set secure bits: %s", strerror(errno)));
    }

    current_caps.Modify(CAP_SETGID);
    current_caps.Modify(CAP_SETUID);
    drop_gid_cap = true;
    drop_uid_cap = true;

    for (auto cap : req.capabilities().capabilities()) {
      int cap_number = 0;
      auto number_or_status = adastra::CapabilitySet::CapabilityFromString(cap);
      if (number_or_status.ok()) {
        cap_number = *number_or_status;
      } else {
        // Allow #<int> for a capability number for which we don't have a
        // name.
        if (cap[0] == '#') {
          int num = 0;
          bool ok = absl::SimpleAtoi(cap.substr(1), &num);
          if (!ok) {
            return absl::InternalError(
                absl::StrFormat("Malformed capability number %s", cap));
          }
          cap_number = num;
        } else {
          return absl::InternalError(
              absl::StrFormat("Unknown capability %s", cap));
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
      return absl::InternalError(absl::StrFormat(
          "Failed to set capabilities: %s", status.ToString()));
    }
  }

  // Groups must be set while we can still change them, before the user.
  bool changed_ids = false;
  if (!req.user().empty()) {
    if (setgroups(groups.size(), groups.data()) == -1) {
      return absl::InternalError(
          absl::StrFormat("Failed to setgroups: %s", strerror(errno)));
    }
  }
  if (!req.user().empty() || !req.group().empty()) {
    if (setgid(gid) == -1) {
      return absl::InternalError(
          absl::StrFormat("Failed to setgid: %s", strerror(errno)));
    }
    changed_ids = true;
  }
  if (!req.user().empty()) {
    if (setuid(uid) == -1) {
      return absl::InternalError(
          absl::StrFormat("Failed to setuid: %s", strerror(errno)));
    }
  }

  if (drop_gid_cap || drop_uid_cap) {
    if (drop_gid_cap) {
      current_caps.Modify(CAP_SETGID, false);
    }
    if (drop_uid_cap) {
      current_caps.Modify(CAP_SETUID, false);
    }
    if (absl::Status status = current_caps.Set(); !status.ok()) {
      return absl::InternalError(
          absl::StrFormat("Failed to drop id caps: %s", status.ToString()));
    }
  }

  // Changing ids clears the dumpable flag, and with no execve to reset it the
  // process would get no core dumps and refuse same-user ptrace.
  if (changed_ids && prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) == -1) {
    return absl::InternalError(
        absl::StrFormat("Failed to set dumpable: %s", strerror(errno)));
  }
#endif // defined(__linux__)
  return absl::OkStatus();
}

absl::Status ZygoteCore::HandleSpawn(const control::SpawnRequest &req,
                                     control::SpawnResponse *resp,
                                     std::vector<toolbelt::FileDescriptor> &fds,
                                     co::Coroutine *c) {
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

  auto local_symbols = std::make_unique<SymbolTable>(&global_symbols_);
  for (auto &var : req.vars()) {
    local_symbols->AddSymbol(var.name(), var.value(), var.exported());
  }

  // It's better to look for the DSO now as reporting an error later
  // is after the fork and this isn't as good for the user.
  if (!req.dso().empty()) {
    struct stat st;
    std::string exe = local_symbols->ReplaceSymbols(req.dso());

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
    std::string expanded_main = local_symbols->ReplaceSymbols(req.main_func());
    void *main_func = dlsym(nullptr, expanded_main.c_str());
    if (main_func == nullptr) {
      return absl::InternalError(
          absl::StrFormat("Failed to find main function %s", expanded_main));
    }
  }

  pid_t pid;
// All looks good for the attempt to fork the zygote.  Let's do it.
#if defined(__linux__)
  // On Linux we can use clone3 instead of fork if we have any namespace
  // assignments.
  if (req.has_ns()) {
    adastra::Namespace ns;
    ns.FromProto(req.ns());
    struct clone_args args = {
        .flags = static_cast<uint64_t>(ns.CloneType()),
        // All other members are zero.
    };
    pid = syscall(__NR_clone3, &args, sizeof(args));
  } else {
    pid = fork();
  }
#else
  pid = fork();
#endif
  if (pid == -1) {
    return absl::InternalError(
        absl::StrFormat("Failed to fork: %s", strerror(errno)));
  }
  if (pid == 0) {
    // Child.
    // Close control socket.
    control_socket_.reset();

    local_symbols->AddSymbol("pid", absl::StrFormat("%d", getpid()), false);

    // We want to close all file descriptors that the zygote has opened except
    // all the ones we want.
    absl::flat_hash_set<int> fds_to_keep_open;

    // Keep 0, 1 and 2 open.  If the stream config specifies that they
    // should be closed, they will be removed from the hash set.
    for (int i = 0; i < 3; i++) {
      fds_to_keep_open.insert(i);
    }

    // The notify fd is added as an exported symbol.
    unsetenv("STAGEZERO_NOTIFY_FD");
    if (req.has_notify_fd_index()) {
      toolbelt::FileDescriptor &notify_fd = fds[req.notify_fd_index()];
      fds_to_keep_open.insert(notify_fd.Fd());

      // The zygote's notify fd is in the current environment and we will
      // replace it.
      std::string notify_fd_str = absl::StrFormat("%d", notify_fd.Fd());
      local_symbols->AddSymbol("STAGEZERO_NOTIFY_FD", notify_fd_str, true);
      setenv("STAGEZERO_NOTIFY_FD", notify_fd_str.c_str(), 1);
    }

    unsetenv("STAGEZERO_PARAMETERS_FDS");
    int parameters_read_fd = -1, parameters_write_fd = -1,
        parameters_events_fd = -1;
    if (req.has_parameters_read_fd_index()) {
      toolbelt::FileDescriptor &parameters_fd =
          fds[req.parameters_read_fd_index()];
      parameters_read_fd = parameters_fd.Fd();
      fds_to_keep_open.insert(parameters_fd.Fd());
    }
    if (req.has_parameters_write_fd_index()) {
      toolbelt::FileDescriptor &parameters_fd =
          fds[req.parameters_write_fd_index()];
      parameters_write_fd = parameters_fd.Fd();
      fds_to_keep_open.insert(parameters_fd.Fd());
    }
    if (req.has_parameters_events_fd_index()) {
      toolbelt::FileDescriptor &parameters_fd =
          fds[req.parameters_events_fd_index()];
      parameters_events_fd = parameters_fd.Fd();
      fds_to_keep_open.insert(parameters_fd.Fd());
    }

    if (parameters_read_fd != -1 && parameters_write_fd != -1 &&
        parameters_events_fd != -1) {
      std::string parameters_fds_str =
          absl::StrFormat("%d:%d:%d", parameters_read_fd, parameters_write_fd,
                          parameters_events_fd);
      local_symbols->AddSymbol("STAGEZERO_PARAMETERS_FDS", parameters_fds_str,
                               true);
      setenv("STAGEZERO_PARAMETERS_FDS", parameters_fds_str.c_str(), 1);
    }

    unsetenv("STAGEZERO_TELEMETRY_FDS");
    int telemetry_read_fd = -1, telemetry_write_fd = -1;
    if (req.has_telemetry_read_fd_index()) {
      toolbelt::FileDescriptor &telemetry_fd =
          fds[req.telemetry_read_fd_index()];
      telemetry_read_fd = telemetry_fd.Fd();
      fds_to_keep_open.insert(telemetry_fd.Fd());
    }
    if (req.has_telemetry_write_fd_index()) {
      toolbelt::FileDescriptor &telemetry_fd =
          fds[req.telemetry_write_fd_index()];
      telemetry_write_fd = telemetry_fd.Fd();
      fds_to_keep_open.insert(telemetry_fd.Fd());
    }
    if (telemetry_read_fd != -1 && telemetry_write_fd != -1) {
      std::string telemetry_fds_str =
          absl::StrFormat("%d:%d", telemetry_read_fd, telemetry_write_fd);
      local_symbols->AddSymbol("STAGEZERO_TELEMETRY_FDS", telemetry_fds_str,
                               true);
      setenv("STAGEZERO_TELEMETRY_FDS", telemetry_fds_str.c_str(), 1);
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
            open(local_symbols->ReplaceSymbols(filename).c_str(), oflag, 0777);
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

    std::vector<int> allfds = scheduler_.GetAllFds();
    for (auto fd : allfds) {
      fds_to_keep_open.insert(fd);
    }

    // Close all open file descriptors that we don't want to explicitly
    // keep open.
    CloseFdsExcept(fds_to_keep_open);

    // We are in the child process running in the server coroutine context.
    // We want to invoke the virtual process's main function using the process's
    // main stack instead of the coroutine stack.  To do this, we stop the
    // coroutine scheduler in the child process and yield the server
    // coroutine.  This will switch to the scheduler's context which will
    // return from Run.
    after_fork_.req = std::move(req);
    after_fork_.local_symbols = std::move(local_symbols);
    forked_ = true;

    // If we are root, set the things that root can set.  Otherwise we ignore
    // the settings.
    if (getuid() == 0) {
      // We are in a forked copy of the zygote and must not return into its
      // server loop.
      if (absl::Status status = SetUpChildAsRoot(req); !status.ok()) {
        std::cerr << "Failed to set up virtual process " << req.name() << ": "
                  << status.ToString() << std::endl;
        _exit(1);
      }
    }

    // Shutdown the telemetry in the zygote fork.
    if (telemetry_.IsOpen()) {
      telemetry_.Shutdown();
    }
    // Stop the coroutine scheduler in the child process.  This will cause
    // a return from its Run() function in ZygoteCore::Run, where we will
    // invoke the main function on the program stack.
    scheduler_.Stop();
    c->YieldToScheduler();

    // Won't get here in the child process.
    abort();
  }

  for (auto &stream : req.streams()) {
    if (stream.has_filename() || stream.close()) {
      continue;
    }
    if (stream.index() < fds.size()) {
      fds[stream.index()].Reset();
    }
  }

  // Close this end of all the fds that the child has duplicated.
  if (req.has_notify_fd_index()) {
    fds[req.notify_fd_index()].Reset();
  }
  if (req.has_parameters_read_fd_index()) {
    fds[req.parameters_read_fd_index()].Reset();
  }
  if (req.has_parameters_write_fd_index()) {
    fds[req.parameters_write_fd_index()].Reset();
  }
  if (req.has_parameters_events_fd_index()) {
    fds[req.parameters_events_fd_index()].Reset();
  }
  if (req.has_telemetry_read_fd_index()) {
    fds[req.telemetry_read_fd_index()].Reset();
  }
  if (req.has_telemetry_write_fd_index()) {
    fds[req.telemetry_write_fd_index()].Reset();
  }
  resp->set_pid(pid);
  fds.clear();

#ifdef __linux__
  int pidfd = pidfd_open(pid, 0);
  fds.push_back(toolbelt::FileDescriptor(pidfd));
  resp->set_pidfd_index(0);
#endif
  logger_.Log(toolbelt::LogLevel::kDebug,
              "Zygote spawned virtual process %s with pid %d",
              req.name().c_str(), pid);
  return absl::OkStatus();
}

// This is called in the child process and does not return.
void ZygoteCore::InvokeMainAfterSpawn(
    std::string exe, const control::SpawnRequest &&req,
    std::unique_ptr<SymbolTable> local_symbols) {

  void *handle = RTLD_DEFAULT;
  if (!req.dso().empty()) {
    exe = local_symbols->ReplaceSymbols(req.dso());
    handle = dlopen(exe.c_str(), RTLD_LAZY);
    if (handle == nullptr) {
      std::cerr << "Failed to open DSO " << req.dso() << std::endl;
      exit(1);
    }
  }
  void *main_func =
      dlsym(handle, local_symbols->ReplaceSymbols(req.main_func()).c_str());
  if (main_func == nullptr) {
    std::cerr << "Failed to find main function " << req.main_func()
              << std::endl;
    exit(1);
  }

  // Build argv and env for main.

  std::vector<std::string> args;
  args.reserve(req.args().size());

  for (auto &arg : req.args()) {
    args.push_back(local_symbols->ReplaceSymbols(arg));
  }

  std::vector<const char *> argv;
  argv.push_back(exe.c_str()); // Executable.
  for (auto &arg : args) {
    argv.push_back(arg.c_str());
  }

  absl::flat_hash_map<std::string, Symbol *> env_vars =
      local_symbols->GetEnvironmentSymbols();
  for (auto &[name, symbol] : env_vars) {
    setenv(name.c_str(), symbol->Value().c_str(), 1);
  }

  signal(SIGPIPE, SIG_IGN);

  // Convert to a function pointer and call main.
  // This is calling a C ABI function.
  using Ptr = int (*)(const char *, int, int, const char **, char **);
  Ptr main = reinterpret_cast<Ptr>(main_func);

  // It seems like passing a SymbolTable object to a loaded shared object
  // doesn't work, at least in optimized mode on Aarch64.  The symptom
  // is that the FindSymbol function fails to search the flat_hash_map.
  // No idea why, but instead of passing the symbol table, we encode
  // it into a string and pass the raw pointer and length.  The function
  // being invoked has C linkage so passing std::string to it might be
  // a bad idea.
  std::stringstream encoded_symbols;
  local_symbols->Encode(encoded_symbols);
  local_symbols.reset();

  exit(main(encoded_symbols.str().data(), int(encoded_symbols.str().size()),
            argv.size(), argv.data(), environ));
}

} // namespace adastra::stagezero
