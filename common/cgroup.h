#pragma once

#include "proto/config.pb.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <google/protobuf/util/message_differencer.h>
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/ascii.h"

// Parts of this file are provided by Cruise LLC.

namespace adastra {

// For documentation on cgroups see:
// https://docs.kernel.org/admin-guide/cgroup-v2.html
enum class CgroupType {
  kDomain,
  kDomainThreaded,
  kThreaded,
};

enum class SubtreeControl {
  kDefault, // Not written to cgroup.subtree_control.
  kEnable,  // Written to cgroup.subtree_control with a '+' prefix.
  kDisable  // Written to cgroup.subtree_control with a '-' prefix.
};

enum class SubtreeCategory {
  kDefault,
  kCpu,
  kCpuset,
  kMemory,
  kIo,
  kPids,
  kRdma
};

struct CgroupCpuController {
  std::optional<int32_t> weight;
  std::optional<int32_t> weight_nice;
  std::optional<int32_t> max;
  std::optional<int32_t> max_burst;
  std::optional<float> uclamp_min;
  std::optional<float> uclamp_max;
  std::optional<int32_t> idle;
  SubtreeControl subtree_control = SubtreeControl::kDefault;
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
  SubtreeControl subtree_control = SubtreeControl::kDefault;

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
  SubtreeControl subtree_control = SubtreeControl::kDefault;
  void ToProto(stagezero::config::Cgroup::CpusetController *dest) const;
  void FromProto(const stagezero::config::Cgroup::CpusetController &src);
};

struct CgroupIOController {
  std::optional<int32_t> weight;
  std::optional<std::string> max;
  SubtreeControl subtree_control = SubtreeControl::kDefault;
  void ToProto(stagezero::config::Cgroup::IOController *dest) const;
  void FromProto(const stagezero::config::Cgroup::IOController &src);
};

struct CgroupPIDController {
  std::optional<int32_t> max;
  SubtreeControl subtree_control = SubtreeControl::kDefault;
  void ToProto(stagezero::config::Cgroup::PIDController *dest) const;
  void FromProto(const stagezero::config::Cgroup::PIDController &src);
};

struct CgroupRDMAController {
  struct Device {
    std::string name;
    int64_t hca_handle;
    std::optional<int64_t> hca_object;
  };

  std::vector<Device> devices;

  SubtreeControl subtree_control = SubtreeControl::kDefault;
  void ToProto(stagezero::config::Cgroup::RDMAController *dest) const;
  void FromProto(const stagezero::config::Cgroup::RDMAController &src);
};

struct Cgroup {
  CgroupType type;
  std::string name;
  std::shared_ptr<CgroupCpusetController> cpuset;
  std::shared_ptr<CgroupCpuController> cpu;
  std::shared_ptr<CgroupMemoryController> memory;
  std::shared_ptr<CgroupIOController> io;
  std::shared_ptr<CgroupPIDController> pids;
  std::shared_ptr<CgroupRDMAController> rdma;

  void ToProto(stagezero::config::Cgroup *dest) const;
  void FromProto(const stagezero::config::Cgroup &src);

  friend bool operator==(const Cgroup &lhs, const Cgroup &rhs) {
    stagezero::config::Cgroup lhsProto;
    lhs.ToProto(&lhsProto);

    stagezero::config::Cgroup rhsProto;
    rhs.ToProto(&rhsProto);

    return google::protobuf::util::MessageDifferencer::Equals(lhsProto,
                                                              rhsProto);
  }
  friend bool operator!=(const Cgroup &lhs, const Cgroup &rhs) {
    return !(lhs == rhs);
  }
};

// Holds the details of the assignment of cgroups to computes.  Each compute
// has a set of cgroups assigned to it.
struct CgroupAssignment {
    std::string compute;
    std::vector<Cgroup> cgroups;

    void FromProto(const stagezero::config::CgroupAssignment& proto) {
        compute = proto.compute();
        for (const auto& cgroup : proto.cgroups()) {
            Cgroup c;
            c.FromProto(cgroup);
            cgroups.push_back(std::move(c));
        }
    }
    void ToProto(stagezero::config::CgroupAssignment* proto) const {
        proto->set_compute(compute);
        for (const auto& cgroup : cgroups) {
            cgroup.ToProto(proto->add_cgroups());
        }
    }
};

// Manages the content of a 'cgroup.subtree_control' file
// This informs the files that are generated for child cgroups
//
// For example:
// 1. Write '+cpuset +cpu' to cgroup.subtree_control'
// 2. The OS processes the change, file reflects 'cpuset cpu' (no + symbols)
// 3. On child cgroup directory creation, the OS will create files like 'cpu.max'
//
struct SubtreeControlSettings {
    SubtreeControl cpu = SubtreeControl::kDefault;
    SubtreeControl cpuset = SubtreeControl::kDefault;
    SubtreeControl memory = SubtreeControl::kDefault;
    SubtreeControl io = SubtreeControl::kDefault;
    SubtreeControl pids = SubtreeControl::kDefault;
    SubtreeControl rdma = SubtreeControl::kDefault;

    static absl::StatusOr<SubtreeControlSettings> FromFile(const std::filesystem::path& file);
    static SubtreeControlSettings ForCgroup(const Cgroup& cgroup);

    std::string ToString() const;
    absl::Status
    Write(const std::filesystem::path& file) const;

    friend bool operator==(const SubtreeControlSettings& lhs, const SubtreeControlSettings& rhs);
    friend bool operator!=(const SubtreeControlSettings& lhs, const SubtreeControlSettings& rhs);
    friend std::ostream& operator<<(std::ostream& os, const SubtreeControlSettings& settings);

    // The files that should be created in a child directory based on these settings
    std::vector<std::string> GetExpectedSubtreeFiles() const;
};

void EnableSubtreeControl(std::stringstream& subtree_control, const std::string& controller);

void DisableSubtreeControl(std::stringstream& subtree_control, const std::string& controller);

void HandleSubtreeControl(
        std::stringstream& subtree_control,
        const std::string& controller,
        SubtreeControl control);

std::vector<std::string> Split(const std::string& text, char delimiter);

} // namespace adastra
