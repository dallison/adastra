#include "stagezero/cgroup.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/strip.h"
#include "toolbelt/fd.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#if defined(__linux__)
#include <sys/inotify.h>
#include <sys/stat.h>
#include <chrono>
#endif

// Some of this code is provided by Cruise LLC.

namespace adastra::stagezero {

  static std::string_view SanitizeCgroupName(std::string_view cgroup) {
  return absl::StripPrefix(absl::StripSuffix(cgroup, "/"), "/");
}


#if defined(__linux__)
using namespace std::chrono_literals;

static absl::Status WriteFile(std::filesystem::path file,
                              std::optional<std::string> line) {
  if (!line.has_value()) {
    return absl::OkStatus();
  }
  std::ofstream out(file);
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write to file %s", file));
  }
  out << *line << std::endl;
  return absl::OkStatus();
}

static absl::Status WriteFile(std::filesystem::path file,
                              std::optional<int32_t> v) {
  if (!v.has_value()) {
    return absl::OkStatus();
  }
  std::ofstream out(file);
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write to file %s", file));
  }
  out << *v << std::endl;
  return absl::OkStatus();
}

static absl::Status WriteFile(std::filesystem::path file,
                              std::optional<int64_t> v) {
  if (!v.has_value()) {
    return absl::OkStatus();
  }
  std::ofstream out(file);
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write to file %s", file));
  }
  out << *v << std::endl;
  return absl::OkStatus();
}

static absl::Status WriteFile(std::filesystem::path file,
                              std::optional<float> v) {
  if (!v.has_value()) {
    return absl::OkStatus();
  }
  std::ofstream out(file);
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write to file %s", file));
  }
  out << *v << std::endl;
  return absl::OkStatus();
}

// Write a field to a file.
#define W(obj, field, file)                                                    \
  if (absl::Status status = WriteFile(cgroup / #obj "." #file, obj.field);     \
      !status.ok()) {                                                          \
    return status;                                                             \
  }

static absl::Status SetCpuController(std::filesystem::path cgroup,
                                     const CgroupCpuController &cpu) {

  W(cpu, weight, weight);
  W(cpu, weight_nice, weight.nice);
  W(cpu, max, max);
  W(cpu, max_burst, max);
  W(cpu, uclamp_min, uclamp.min);
  W(cpu, uclamp_max, uclamp.max);
  W(cpu, idle, idle);

  return absl::OkStatus();
}

static absl::Status SetCpusetController(std::filesystem::path cgroup,
                                        const CgroupCpusetController &cpuset) {
  W(cpuset, cpus, cpus);
  W(cpuset, mems, mems);
  W(cpuset, cpus_exclusive, cpus.exclusive);

  if (cpuset.partition.has_value()) {
    switch (cpuset.partition.value()) {
    case CgroupCpusetController::Partition::kMember:
      if (absl::Status status =
              WriteFile(cgroup / "cpuset.cpus.partition", "member");
          !status.ok()) {
        return status;
      }
      break;
    case CgroupCpusetController::Partition::kRoot:
      W(cpuset, mems, mems);

      if (absl::Status status =
              WriteFile(cgroup / "cpuset.cpuspartition", "root");
          !status.ok()) {
        return status;
      }
      break;
    case CgroupCpusetController::Partition::kIsolated:
      W(cpuset, mems, mems);

      if (absl::Status status =
              WriteFile(cgroup / "cpuset.cpus.partition", "isolated");
          !status.ok()) {
        return status;
      }
      break;
    }
  }
  return absl::OkStatus();
}

static absl::Status SetMemoryController(std::filesystem::path cgroup,
                                        const CgroupMemoryController &memory) {
  W(memory, min, min);
  W(memory, low, low);
  W(memory, high, high);
  W(memory, max, max);
  W(memory, oom_group, oom.group);
  W(memory, swap_high, swap.high);
  W(memory, swap_max, swap.max);
  W(memory, zswap_max, zswap.max);
  W(memory, zswap_writeback, zswap.writeback);
  return absl::OkStatus();
}

static absl::Status SetIOController(std::filesystem::path cgroup,
                                    const CgroupIOController &io) {
  W(io, weight, weight);
  W(io, max, max);
  return absl::OkStatus();
}

static absl::Status SetPIDController(std::filesystem::path cgroup,
                                     const CgroupPIDController &pid) {
  if (!pid.max.has_value()) {
    if (absl::Status status = WriteFile(cgroup / "pids.max", "max");
        !status.ok()) {
      return status;
    }
    return absl::OkStatus();
  }
  W(pid, max, max);
  return absl::OkStatus();
}

static absl::Status SetRDMAController(std::filesystem::path cgroup,
                                      const CgroupRDMAController &rdma) {
  std::filesystem::path file = cgroup / "rdma.max";
  std::ofstream out(file);
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write to file %s", file));
  }
  for (auto &dev : rdma.devices) {
    std::string line;
    if (dev.hca_object.has_value()) {
      line = absl::StrFormat("%s hca_handle=%d hca_object=%d", dev.name,
                             dev.hca_handle, dev.hca_object.value());
    } else {
      line = absl::StrFormat("%s hca_handle=%d hca_object=max", dev.name,
                             dev.hca_handle);
    }
    out << line << std::endl;
  }
  return absl::OkStatus();
}

#undef W

// This code is provided by Cruise LLC.
static std::vector<std::string>
GetMissingFiles(const std::filesystem::path &directory,
                std::vector<std::string> expectedBasenames) {
  std::vector<std::string> missing;

  for (const auto &basename : expectedBasenames) {
    const auto fullPath = directory / basename;
    if (!std::filesystem::exists(fullPath)) {
      missing.emplace_back(fullPath.string());
    }
  }

  return missing;
}

static absl::Status
EnsureChildCgroupReady(const std::filesystem::path &cgroup_path,
                       const SubtreeControlSettings &parentSettings,
                       co::Coroutine *c) {

  const auto expectedFiles = parentSettings.GetExpectedSubtreeFiles();

  // The child directory has already been created.
  // This means the OS may populate it at any moment now.
  //
  // Now that it's been created, we can also begin to inotify watch it.
  // However, it also takes us a moment to set up the file descriptor and begin
  // polling.
  //
  // Check if the OS has beat us, or go into a polling loop if necessary.
  auto waitingOn = GetMissingFiles(cgroup_path, expectedFiles);
  if (waitingOn.empty()) {
    return absl::OkStatus();
  }

  if (c) {
    auto ifd = toolbelt::FileDescriptor(inotify_init());
    if (!ifd.Valid()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to create inotify fd to monitor directory '%s': %s",
          cgroup_path, strerror(errno)));
    }
    auto wfd = toolbelt::FileDescriptor(inotify_add_watch(
        ifd.Fd(), cgroup_path.c_str(), IN_MODIFY | IN_CREATE | IN_DELETE));
    if (!wfd.Valid()) {
      return absl::InternalError(absl::StrFormat(
          "Failed to add watch fd to monitor directory '%s': %s",
          strerror(errno)));
    }

    const auto pollFrequency = 20s;
    const auto maxAttempts = 25;
    int attempt = 0;

    while (!waitingOn.empty() && attempt < maxAttempts) {
            c->Wait(ifd.Fd(), EPOLLIN, pollFrequency.count());
            // Something in the directory has changed, or we timed out waiting
            // for events. We could use the event to determine exactly what has
            // updated and what remains. But let's re-query anyway in case we
            // missed an event while setting up poll
            waitingOn = GetMissingFiles(cgroup_path, expectedFiles);
            attempt++;
    }
    if (waitingOn.empty()) {
      return absl::OkStatus();
    }
  }

  std::sort(waitingOn.begin(), waitingOn.end());
  return absl::InternalError(
      absl::StrFormat("Cgroup directory '%s' is not ready, missing files: %s",
                      cgroup_path, absl::StrJoin(waitingOn, ",")));
}

static absl::Status ConfigureRootCgroup(const Cgroup &cgroup,
                                        const std::string &cgroup_root_dir) {
  // Configure subtree_control settings for (future) children
  const auto subtreeControlSettings = SubtreeControlSettings::ForCgroup(cgroup);
  const auto subtreeFile =
      absl::StrFormat("%s/cgroup.subtree_control", cgroup_root_dir);
  return subtreeControlSettings.Write(subtreeFile);
}

static absl::Status ConfigureChildCgroup(const Cgroup &cgroup,
                                         const std::string &cgroup_root_dir,
                                         co::Coroutine *c) {
  std::filesystem::path cgroup_path(absl::StrFormat(
      "%s/%s", cgroup_root_dir, SanitizeCgroupName(cgroup.name)));
  std::error_code ec;
  std::filesystem::create_directories(cgroup_path, ec);
  if (ec) {
    return absl::InternalError(
        absl::StrFormat("Failed to create cgroup directory %s: %s",
                        cgroup_path.string(), ec.message()));
  }

  // The OS will generate files according to the parent's subtree_control
  // settings. This takes place after we requested the directory be created,
  // above.
  //
  // With the directory created, we would like to write content to the child
  // group files. However, we do not have file create permissions: only
  // modifying is allowed in this area.
  //
  // We must wait for the OS to produce the files before proceeding.
  // In practice, this is almost instantaneous but we have to prevent an access
  // race here
  const auto parentSubtreeFile =
      cgroup_path.parent_path() / "cgroup.subtree_control";
  auto parentSubtreeSettings =
      SubtreeControlSettings::FromFile(parentSubtreeFile);
  if (!parentSubtreeSettings.ok()) {
    return parentSubtreeSettings.status();
  }
  if (auto status =
          EnsureChildCgroupReady(cgroup_path, *parentSubtreeSettings, c);
      !status.ok()) {
    return status;
  }

  // With the child directory provisioned, begin configuring its settings
  const auto subtreeFile =
      absl::StrFormat("%s/cgroup.subtree_control", cgroup_path);
  const auto subtreeControlSettings = SubtreeControlSettings::ForCgroup(cgroup);
  if (auto status = subtreeControlSettings.Write(subtreeFile); !status.ok()) {
    return status;
  }
  // Write the type to cgroup.type.
  std::ofstream out(cgroup_path / "cgroup.type");
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write cgroup.type: %s", strerror(errno)));
  }
  switch (cgroup.type) {
  case CgroupType::kDomain:
    out << "domain\n";
    break;
  case CgroupType::kDomainThreaded:
    out << "domain threaded\n";
    break;
  case CgroupType::kThreaded:
    out << "threaded\n";
    break;
  }
  out.close();

  // Write all the cgroup controllers.
  if (cgroup.cpu != nullptr) {
    if (absl::Status status = SetCpuController(cgroup_path, *cgroup.cpu);
        !status.ok()) {
      return status;
    }
  }

  if (cgroup.cpuset != nullptr) {
    if (absl::Status status = SetCpusetController(cgroup_path, *cgroup.cpuset);
        !status.ok()) {
      return status;
    }
  }
  if (cgroup.memory != nullptr) {
    if (absl::Status status = SetMemoryController(cgroup_path, *cgroup.memory);
        !status.ok()) {
      return status;
    }
  }
  if (cgroup.io != nullptr) {
    if (absl::Status status = SetIOController(cgroup_path, *cgroup.io);
        !status.ok()) {
      return status;
    }
  }
  if (cgroup.pids != nullptr) {
    if (absl::Status status = SetPIDController(cgroup_path, *cgroup.pids);
        !status.ok()) {
      return status;
    }
  }
  if (cgroup.rdma != nullptr) {
    if (absl::Status status = SetRDMAController(cgroup_path, *cgroup.rdma);
        !status.ok()) {
      return status;
    }
  }
  return absl::OkStatus();
}
#endif // defined(__linux__)

absl::Status CreateCgroup(const Cgroup &cgroup,
                          const std::string &cgroup_root_dir,
                          toolbelt::Logger &logger, co::Coroutine *c) {
  bool cgroups_supported = true;
#if !defined(__linux__)
  cgroups_supported = false;
#endif
  if (!cgroups_supported) {
    logger.Log(
        toolbelt::LogLevel::kInfo,
        "Cgroups are not supported on this OS; creation of cgroup '%s' ignored",
        cgroup.name.c_str());
    return absl::OkStatus();
  }
  if (geteuid() != 0) {
    // Not root.
    logger.Log(toolbelt::LogLevel::kInfo,
               "Not running as root; creation of cgroup '%s' ignored",
               cgroup.name.c_str());
    return absl::OkStatus();
  }
#if defined(__linux__)
  std::error_code ec;
  std::filesystem::create_directories(cgroup_root_dir, ec);
  if (ec) {
    return absl::InternalError(
        absl::StrFormat("Failed to create cgroup directory %s: %s",
                        cgroup_root_dir, ec.message()));
  }

  if (cgroup.name.empty() || cgroup.name == "/") {
    return ConfigureRootCgroup(cgroup, cgroup_root_dir);
  }
  return ConfigureChildCgroup(cgroup, cgroup_root_dir, c);
#else
  // For non-Linux systems, we do not support cgroups.
  return absl::OkStatus();
#endif
}

#if defined(__linux__)
static absl::Status RemoveRootCgroup(const std::string &cgroup_root_dir) {
  // We cannot actually remove the root cgroup directory.
  // However, we can still update any existing file content
  const SubtreeControlSettings subtreeControlDefaults;
  const auto subtreeFile =
      absl::StrFormat("%s/cgroup.subtree_control", cgroup_root_dir);
  return subtreeControlDefaults.Write(subtreeFile);
}

static absl::Status RemoveChildCgroup(const std::string &cgroup,
                                      const std::string &cgroup_root_dir) {
  std::filesystem::path cgroup_path(
      absl::StrFormat("%s/%s", cgroup_root_dir, SanitizeCgroupName(cgroup)));
  if (!std::filesystem::exists(cgroup_path)) {
    return absl::NotFoundError(
        absl::StrFormat("Cgroup %s does not exist", cgroup_path.string()));
  }
  // Use rmdir to remove the cgroup directory.
  // This will work even if the directory is not empty, as long as the
  // cgroup filesystem supports it.
  if (int err = ::rmdir(cgroup_path.c_str()); err != 0) {
    return absl::InternalError(absl::StrFormat("Failed to remove cgroup %s: %s",
                                               cgroup_path.string(),
                                               strerror(errno)));
  }
  return absl::OkStatus();
}
#endif // defined(__linux__)

absl::Status RemoveCgroup(const std::string &cgroup,
                          const std::string &cgroup_root_dir,
                          toolbelt::Logger &logger) {
  bool cgroups_supported = true;
#if !defined(__linux__)
  cgroups_supported = false;
#endif
  if (!cgroups_supported) {
    logger.Log(
        toolbelt::LogLevel::kInfo,
        "Cgroups are not supported on this OS; removal of cgroup '%s' ignored",
        cgroup.c_str());
    return absl::OkStatus();
  }
  if (geteuid() != 0) {
    // Not root.
    logger.Log(toolbelt::LogLevel::kInfo,
               "Not running as root; removal of cgroup '%s' ignored",
               cgroup.c_str());
    return absl::OkStatus();
  }
#if defined(__linux__)
  if (cgroup.empty() || cgroup == "/") {
    return RemoveRootCgroup(cgroup_root_dir);
  }
  return RemoveChildCgroup(cgroup, cgroup_root_dir);
#else
  // For non-Linux systems, we do not support cgroups.
  return absl::OkStatus();
#endif
}

absl::Status AddToCgroup(const std::string &proc, const std::string &cgroup,
                         int pid, const std::string &cgroup_root_dir,
                         toolbelt::Logger &logger) {
  bool cgroups_supported = true;
#if !defined(__linux__)
  cgroups_supported = false;
#endif
  if (!cgroups_supported) {
    logger.Log(toolbelt::LogLevel::kInfo,
               "Cgroups are not supported on this OS; addition of process %s "
               "with pid %d to cgroup '%s' ignored",
               proc.c_str(), pid, cgroup.c_str());

    return absl::OkStatus();
  }
  if (geteuid() != 0) {
    // Not root.
    logger.Log(toolbelt::LogLevel::kInfo,
               "Not running as root; addition of process %s with pid %d to "
               "cgroup '%s' ignored",
               proc.c_str(), pid, cgroup.c_str());

    return absl::OkStatus();
  }
  std::filesystem::path cgroup_path(
      absl::StrFormat("%s/%s", cgroup_root_dir, SanitizeCgroupName(cgroup)));

  std::ofstream out(cgroup_path / "cgroup.procs");
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write cgroup.procs: %s", strerror(errno)));
  }
  out << pid << std::endl;
  return absl::OkStatus();
}

absl::Status FreezeCgroup(const std::string &cgroup,
                          const std::string &cgroup_root_dir,
                          toolbelt::Logger &logger) {
  bool cgroups_supported = true;
#if !defined(__linux__)
  cgroups_supported = false;
#endif
  if (!cgroups_supported) {
    logger.Log(
        toolbelt::LogLevel::kInfo,
        "Cgroups are not supported on this OS; freezing of cgroup '%s' ignored",
        cgroup.c_str());

    return absl::OkStatus();
  }
  if (geteuid() != 0) {
    // Not root.
    logger.Log(toolbelt::LogLevel::kInfo,
               "Not running as root; freezing of cgroup '%s' ignored",
               cgroup.c_str());

    return absl::OkStatus();
  }
  std::filesystem::path cgroup_path(
      absl::StrFormat("%s/%s", cgroup_root_dir, SanitizeCgroupName(cgroup)));

  std::ofstream out(cgroup_path / "cgroup.freeze");
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write cgroup.freeze: %s", strerror(errno)));
  }
  out << 1 << std::endl;
  return absl::OkStatus();
}

absl::Status ThawCgroup(const std::string &cgroup,
                        const std::string &cgroup_root_dir,
                        toolbelt::Logger &logger) {
  bool cgroups_supported = true;
#if !defined(__linux__)
  cgroups_supported = false;
#endif
  if (!cgroups_supported) {
    logger.Log(
        toolbelt::LogLevel::kInfo,
        "Cgroups are not supported on this OS; thawing of cgroup '%s' ignored",
        cgroup.c_str());

    return absl::OkStatus();
  }
  if (geteuid() != 0) {
    // Not root.
    logger.Log(toolbelt::LogLevel::kInfo,
               "Not running as root; thawing of cgroup '%s' ignored",
               cgroup.c_str());

    return absl::OkStatus();
  }
  std::filesystem::path cgroup_path(
      absl::StrFormat("%s/%s", cgroup_root_dir, SanitizeCgroupName(cgroup)));

  std::ofstream out(cgroup_path / "cgroup.freeze");
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write cgroup.freeze: %s", strerror(errno)));
  }
  out << 0 << std::endl;
  return absl::OkStatus();
}

absl::Status KillCgroup(const std::string &cgroup,
                        const std::string &cgroup_root_dir,
                        toolbelt::Logger &logger) {
  bool cgroups_supported = true;
#if !defined(__linux__)
  cgroups_supported = false;
#endif
  if (!cgroups_supported) {
    logger.Log(
        toolbelt::LogLevel::kInfo,
        "Cgroups are not supported on this OS; killing of cgroup '%s' ignored",
        cgroup.c_str());

    return absl::OkStatus();
  }
  if (geteuid() != 0) {
    // Not root.
    logger.Log(toolbelt::LogLevel::kInfo,
               "Not running as root; freezing of killing '%s' ignored",
               cgroup.c_str());

    return absl::OkStatus();
  }
  std::filesystem::path cgroup_path(
      absl::StrFormat("%s/%s", cgroup_root_dir, SanitizeCgroupName(cgroup)));

  std::ofstream out(cgroup_path / "cgroup.kill");
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write cgroup.kill: %s", strerror(errno)));
  }
  out << 1 << std::endl;
  return absl::OkStatus();
}

} // namespace adastra::stagezero
