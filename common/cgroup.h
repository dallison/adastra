#pragma once

#include "proto/config.pb.h"
#include <cstdint>
#include <string>
#include <memory>
#include <optional>

namespace adastra {

// For documentation on cgroups see:
// https://docs.kernel.org/admin-guide/cgroup-v2.html
enum class CgroupType {
  kDomain,
  kDomainThreaded,
  kThreaded,
};

struct CgroupCpuController {
  std::optional<int32_t> weight;
  std::optional<int32_t> weight_nice;
  std::optional<int32_t> max;
  std::optional<int32_t> max_burst;
  std::optional<float> uclamp_min;
  std::optional<float> uclamp_max;
  std::optional<int32_t> idle;
  void ToProto(stagezero::config::Cgroup::CpuController *dest) const;
  void FromProto(const stagezero::config::Cgroup::CpuController &src);
};

struct CgroupMemoryController {
  std::optional<int64_t> min;
  std::optional<int64_t> low;
  std::optional<int64_t> high;
  std::optional<int64_t> max;
  std::optional<int32_t> oom_group;
  std::optional<int64_t> swap_high;
  std::optional<int64_t> swap_max;
  std::optional<int64_t> zswap_max;
  std::optional<int32_t> zswap_writeback;

  void ToProto(stagezero::config::Cgroup::MemoryController *dest) const;
  void FromProto(const stagezero::config::Cgroup::MemoryController &src);
};

struct CgroupCpusetController {
  enum Partition {
    kMember,
    kRoot,
    kIsolated,
  };

  std::optional<std::string> cpus;
  std::optional<std::string> mems;
  std::optional<std::string> cpus_exclusive;
  std::optional<Partition> partition;
  void ToProto(stagezero::config::Cgroup::CpusetController *dest) const;
  void FromProto(const stagezero::config::Cgroup::CpusetController &src);
};

struct CgroupIOController {
  std::optional<int32_t> weight;
  std::optional<std::string> max;
  void ToProto(stagezero::config::Cgroup::IOController *dest) const;
  void FromProto(const stagezero::config::Cgroup::IOController &src);
};

struct Cgroup {
  CgroupType type;
  std::string name;
  std::shared_ptr<CgroupCpusetController> cpuset;
  std::shared_ptr<CgroupCpuController> cpu;
  std::shared_ptr<CgroupMemoryController> memory;
  std::shared_ptr<CgroupIOController> io;

  void ToProto(stagezero::config::Cgroup *dest) const;
  void FromProto(const stagezero::config::Cgroup &src);
};

} // namespace adastra
