#include "stagezero/cgroup.h"
#include "absl/strings/str_format.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace adastra::stagezero {

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

static absl::Status SetCpuController(std::filesystem::path cgroup,
                                     const CgroupCpuController &cpu) {

  if (absl::Status status = WriteFile(cgroup / "cpu.weight", cpu.weight);
      !status.ok()) {
    return status;
  }

  if (absl::Status status =
          WriteFile(cgroup / "cpu.weight.nice", cpu.weight_nice);
      !status.ok()) {
    return status;
  }
  if (absl::Status status = WriteFile(cgroup / "cpu.max", cpu.max);
      !status.ok()) {
    return status;
  }
  if (absl::Status status = WriteFile(cgroup / "cpu.max.burst", cpu.max_burst);
      !status.ok()) {
    return status;
  }
  if (absl::Status status =
          WriteFile(cgroup / "cpu.uclamp.min", cpu.uclamp_min);
      !status.ok()) {
    return status;
  }
  if (absl::Status status =
          WriteFile(cgroup / "cpu.uclamp.max", cpu.uclamp_max);
      !status.ok()) {
    return status;
  }
  if (absl::Status status = WriteFile(cgroup / "cpu.idle", cpu.idle);
      !status.ok()) {
    return status;
  }
  return absl::OkStatus();
}

static absl::Status SetCpusetController(std::filesystem::path cgroup,
                                        const CgroupCpusetController &cpuset) {
  if (absl::Status status = WriteFile(cgroup / "cpuset.cpus", cpuset.cpus);
      !status.ok()) {
    return status;
  }
  if (absl::Status status = WriteFile(cgroup / "cpuset.mems", cpuset.mems);
      !status.ok()) {
    return status;
  }
  if (absl::Status status =
          WriteFile(cgroup / "cpuset.cpus.exclusive", cpuset.cpus_exclusive);
      !status.ok()) {
    return status;
  }

  if (cpuset.partition.has_value()) {
    switch (cpuset.partition.value()) {
    case CgroupCpusetController::Partition::kMember:
      if (absl::Status status =
              WriteFile(cgroup / "cpuset.partition", "member");
          !status.ok()) {
        return status;
      }
      break;
    case CgroupCpusetController::Partition::kRoot:
      if (absl::Status status = WriteFile(cgroup / "cpuset.partition", "root");
          !status.ok()) {
        return status;
      }
      break;
    case CgroupCpusetController::Partition::kIsolated:
      if (absl::Status status =
              WriteFile(cgroup / "cpuset.partition", "isolated");
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
  if (absl::Status status = WriteFile(cgroup / "memory.min", memory.min);
      !status.ok()) {
    return status;
  }
  if (absl::Status status = WriteFile(cgroup / "memory.low", memory.low);
      !status.ok()) {
    return status;
  }

  if (absl::Status status = WriteFile(cgroup / "memory.high", memory.high);
      !status.ok()) {
    return status;
  }
  if (absl::Status status = WriteFile(cgroup / "memory.max", memory.max);
      !status.ok()) {
    return status;
  }
  if (absl::Status status =
          WriteFile(cgroup / "memory.oom.group", memory.oom_group);
      !status.ok()) {
    return status;
  }
  if (absl::Status status =
          WriteFile(cgroup / "memory.swap.high", memory.swap_high);
      !status.ok()) {
    return status;
  }
  if (absl::Status status =
          WriteFile(cgroup / "memory.swap.max", memory.swap_max);
      !status.ok()) {
    return status;
  }
  if (absl::Status status =
          WriteFile(cgroup / "memory.zswap.max", memory.zswap_max);
      !status.ok()) {
    return status;
  }
  if (absl::Status status =
          WriteFile(cgroup / "memory.zswap.writeback", memory.zswap_writeback);
      !status.ok()) {
    return status;
  }
  return absl::OkStatus();
}

static absl::Status SetIOController(std::filesystem::path cgroup,
                                    const CgroupIOController &io) {
  if (absl::Status status = WriteFile(cgroup / "io.weight", io.weight);
      !status.ok()) {
    return status;
  }
  if (absl::Status status = WriteFile(cgroup / "io.max", io.max);
      !status.ok()) {
    return status;
  }
  return absl::OkStatus();
}

absl::Status CreateCgroup(const Cgroup &cgroup, toolbelt::Logger &logger) {
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
  std::filesystem::path cgroup_path(
      absl::StrFormat("/sys/fs/cgroup/%s", cgroup.name));
  std::error_code error;
  if (!std::filesystem::exists(cgroup_path)) {
    if (!std::filesystem::create_directories(cgroup_path, error)) {
      return absl::InternalError(absl::StrFormat("Failed to create cgroup %s: %s",
                                                 cgroup.name, error.message()));
    }
  }

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

  return absl::OkStatus();
}

absl::Status RemoveCgroup(const std::string &cgroup, toolbelt::Logger &logger) {
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
  return absl::OkStatus();
}

absl::Status AddToCgroup(const std::string &proc, const std::string &cgroup,
                         int pid, toolbelt::Logger &logger) {
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
  return absl::OkStatus();
}

absl::Status RemoveFromCgroup(const std::string &proc,
                              const std::string &cgroup, int pid,
                              toolbelt::Logger &logger) {
  bool cgroups_supported = true;
#if !defined(__linux__)
  cgroups_supported = false;
#endif
  if (!cgroups_supported) {
    logger.Log(toolbelt::LogLevel::kInfo,
               "Cgroups are not supported on this OS; removal of process %s "
               "with pid %d from cgroup '%s' ignored",
               proc.c_str(), pid, cgroup.c_str());
    return absl::OkStatus();
  }
  if (geteuid() != 0) {
    // Not root.
    logger.Log(toolbelt::LogLevel::kInfo,
               "Not running as root;; removal of process %s "
               "with pid %d from cgroup '%s' ignored",
               proc.c_str(), pid, cgroup.c_str());
  }
  return absl::OkStatus();
}

} // namespace adastra::stagezero
