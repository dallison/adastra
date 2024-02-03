#include "common/cgroup.h"

namespace adastra {

void CgroupCpuController::ToProto(
    stagezero::config::Cgroup::CpuController *dest) const {
  if (weight.has_value()) {
    dest->set_weight(weight.value());
  }
  if (weight_nice.has_value()) {
    dest->set_weight_nice(weight_nice.value());
  }
  if (max.has_value()) {
    dest->set_max(max.value());
  }
  if (max_burst.has_value()) {
    dest->set_max_burst(max_burst.value());
  }
  if (uclamp_min.has_value()) {
    dest->set_uclamp_min(uclamp_min.value());
  }
  if (uclamp_max.has_value()) {
    dest->set_uclamp_max(uclamp_max.value());
  }
  if (idle.has_value()) {
    dest->set_idle(idle.value());
  }
}

void CgroupCpuController::FromProto(
    const stagezero::config::Cgroup::CpuController &src) {
  if (src.has_weight()) {
    weight = src.weight();
  }
  if (src.has_weight_nice()) {
    weight_nice = src.weight_nice();
  }
  if (src.has_max()) {
    max = src.max();
  }
  if (src.has_max_burst()) {
    max_burst = src.max_burst();
  }
  if (src.has_uclamp_min()) {
    uclamp_min = src.uclamp_min();
  }
  if (src.has_uclamp_max()) {
    uclamp_max = src.uclamp_max();
  }
  if (src.has_idle()) {
    idle = src.idle();
  }
}

void CgroupMemoryController::ToProto(
    stagezero::config::Cgroup::MemoryController *dest) const {
  if (min.has_value()) {
    dest->set_min(min.value());
  }
  if (low.has_value()) {
    dest->set_low(low.value());
  }
  if (high.has_value()) {
    dest->set_high(high.value());
  }
  if (max.has_value()) {
    dest->set_max(max.value());
  }
  if (oom_group.has_value()) {
    dest->set_oom_group(oom_group.value());
  }
  if (swap_high.has_value()) {
    dest->set_swap_high(swap_high.value());
  }
  if (swap_max.has_value()) {
    dest->set_swap_max(swap_max.value());
  }
  if (zswap_max.has_value()) {
    dest->set_zswap_max(zswap_max.value());
  }
  if (zswap_writeback.has_value()) {
    dest->set_zswap_writeback(zswap_writeback.value());
  }
}

void CgroupMemoryController::FromProto(
    const stagezero::config::Cgroup::MemoryController &src) {
  if (src.has_min()) {
    min = src.min();
  }
  if (src.has_low()) {
    low = src.low();
  }
  if (src.has_high()) {
    high = src.high();
  }
  if (src.has_max()) {
    max = src.max();
  }
  if (src.has_oom_group()) {
    oom_group = src.oom_group();
  }
  if (src.has_swap_high()) {
    swap_high = src.swap_high();
  }
  if (src.has_swap_max()) {
    swap_max = src.swap_max();
  }
  if (src.has_zswap_max()) {
    zswap_max = src.zswap_max();
  }
  if (src.has_zswap_writeback()) {
    zswap_writeback = src.zswap_writeback();
  }
}

void CgroupCpusetController::ToProto(
    stagezero::config::Cgroup::CpusetController *dest) const {
  if (cpus.has_value()) {
    dest->set_cpus(cpus.value());
  }
  if (mems.has_value()) {
    dest->set_mems(mems.value());
  }
  if (cpus_exclusive.has_value()) {
    dest->set_cpus_exclusive(cpus_exclusive.value());
  }
  if (partition.has_value()) {
    switch (partition.value()) {
    case Partition::kMember:
      dest->set_partition(
          stagezero::config::Cgroup::CpusetController::P_MEMBER);
      break;
    case Partition::kRoot:
      dest->set_partition(stagezero::config::Cgroup::CpusetController::P_ROOT);
      break;
    case Partition::kIsolated:
      dest->set_partition(
          stagezero::config::Cgroup::CpusetController::P_ISOLATED);
      break;
    }
  }
}

void CgroupCpusetController::FromProto(
    const stagezero::config::Cgroup::CpusetController &src) {
  if (src.has_cpus()) {
    cpus = src.cpus();
  }
  if (src.has_mems()) {
    mems = src.mems();
  }
  if (src.has_cpus_exclusive()) {
    cpus_exclusive = src.cpus_exclusive();
  }
  if (src.has_partition()) {
    switch (src.partition()) {
    case stagezero::config::Cgroup::CpusetController::P_MEMBER:
      partition = Partition::kMember;
      break;
    case stagezero::config::Cgroup::CpusetController::P_ROOT:
      partition = Partition::kRoot;
      break;
    case stagezero::config::Cgroup::CpusetController::P_ISOLATED:
      partition = Partition::kIsolated;
      break;
    default:
      break;
    }
  }
}

void CgroupIOController::ToProto(
    stagezero::config::Cgroup::IOController *dest) const {
  if (weight.has_value()) {
    dest->set_weight(weight.value());
  }
  if (weight.has_value()) {
    dest->set_max(max.value());
  }
}

void CgroupIOController::FromProto(
    const stagezero::config::Cgroup::IOController &src) {
  if (src.has_weight()) {
    weight = src.weight();
  }
  if (src.has_max()) {
    max = src.max();
  }
}

void Cgroup::ToProto(stagezero::config::Cgroup *dest) const {
  switch (type) {
  case CgroupType::kDomain:
    dest->set_type(stagezero::config::Cgroup::CG_DOMAIN);
    break;
  case CgroupType::kDomainThreaded:
    dest->set_type(stagezero::config::Cgroup::CG_DOMAIN_THREADED);
    break;
  case CgroupType::kThreaded:
    dest->set_type(stagezero::config::Cgroup::CG_THREADED);
    break;
  }
  dest->set_name(name);
  if (cpu != nullptr) {
    cpu->ToProto(dest->mutable_cpu());
  }
  if (cpuset != nullptr) {
    cpuset->ToProto(dest->mutable_cpuset());
  }
  if (memory != nullptr) {
    memory->ToProto(dest->mutable_memory());
  }
  if (io != nullptr) {
    io->ToProto(dest->mutable_io());
  }
}

void Cgroup::FromProto(const stagezero::config::Cgroup &src) {
  switch (src.type()) {
  case stagezero::config::Cgroup::CG_DOMAIN:
    type = CgroupType::kDomain;
    break;
  case stagezero::config::Cgroup::CG_DOMAIN_THREADED:
    type = CgroupType::kDomainThreaded;
    break;
  case stagezero::config::Cgroup::CG_THREADED:
    type = CgroupType::kThreaded;
    break;
  default:
    break;
  }
  name = src.name();
  if (src.has_cpu()) {
    cpu = std::make_shared<CgroupCpuController>();
    cpu->FromProto(src.cpu());
  }
  if (src.has_cpuset()) {
    cpuset = std::make_shared<CgroupCpusetController>();
    cpuset->FromProto(src.cpuset());
  }
  if (src.has_memory()) {
    memory = std::make_shared<CgroupMemoryController>();
    memory->FromProto(src.memory());
  }
  if (src.has_io()) {
    io = std::make_shared<CgroupIOController>();
    io->FromProto(src.io());
  }
}
} // namespace adastra
