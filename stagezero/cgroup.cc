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

// Write a field to a file.
#define W(obj, field, file)                                                    \
  if (absl::Status status = WriteFile(cgroup / #obj "." #file, obj.field);         \
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
              WriteFile(cgroup / "cpuset.partition", "member");
          !status.ok()) {
        return status;
      }
      break;
    case CgroupCpusetController::Partition::kRoot:
      W(cpuset, mems, mems);

      if (absl::Status status = WriteFile(cgroup / "cpuset.partition", "root");
          !status.ok()) {
        return status;
      }
      break;
    case CgroupCpusetController::Partition::kIsolated:
      W(cpuset, mems, mems);

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
    if (absl::Status status = WriteFile(cgroup / "pid.max", "max");
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
      return absl::InternalError(absl::StrFormat(
          "Failed to create cgroup %s: %s", cgroup.name, error.message()));
    }
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
  if (cgroup.pid != nullptr) {
    if (absl::Status status = SetPIDController(cgroup_path, *cgroup.pid);
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
  std::filesystem::path cgroup_path(
      absl::StrFormat("/sys/fs/cgroup/%s", cgroup));

  std::ofstream out(cgroup_path / "cgroup.procs");
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write cgroup.procs: %s", strerror(errno)));
  }
  out << pid << std::endl;
  return absl::OkStatus();
}

absl::Status FreezeCgroup(const std::string &cgroup, toolbelt::Logger &logger) {
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
      absl::StrFormat("/sys/fs/cgroup/%s", cgroup));

  std::ofstream out(cgroup_path / "cgroup.freeze");
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write cgroup.freeze: %s", strerror(errno)));
  }
  out << 1 << std::endl;
  return absl::OkStatus();
}

absl::Status ThawCgroup(const std::string &cgroup, toolbelt::Logger &logger) {
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
      absl::StrFormat("/sys/fs/cgroup/%s", cgroup));

  std::ofstream out(cgroup_path / "cgroup.freeze");
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write cgroup.freeze: %s", strerror(errno)));
  }
  out << 0 << std::endl;
  return absl::OkStatus();
}

absl::Status KillCgroup(const std::string &cgroup, toolbelt::Logger &logger) {
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
      absl::StrFormat("/sys/fs/cgroup/%s", cgroup));

  std::ofstream out(cgroup_path / "cgroup.kill");
  if (!out) {
    return absl::InternalError(
        absl::StrFormat("Failed to write cgroup.kill: %s", strerror(errno)));
  }
  out << 1 << std::endl;
  return absl::OkStatus();
}

} // namespace adastra::stagezero
