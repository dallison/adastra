#include "common/cgroup.h"

namespace adastra {

// ToProto converter.
#define T(field)                                                               \
  if (field.has_value()) {                                                     \
    dest->set_##field(field.value());                                          \
  }

// FromProto converter.
#define F(field)                                                               \
  if (src.has_##field()) {                                                     \
    field = src.field();                                                       \
  }

void CgroupCpuController::ToProto(
    stagezero::config::Cgroup::CpuController *dest) const {
  T(weight);
  T(weight_nice);
  T(max);
  T(max_burst);
  T(uclamp_min);
  T(uclamp_max);
  T(idle);
}

void CgroupCpuController::FromProto(
    const stagezero::config::Cgroup::CpuController &src) {
  F(weight);
  F(weight_nice);
  F(max);
  F(max_burst);
  F(uclamp_min);
  F(uclamp_max);
  F(idle);
}

void CgroupMemoryController::ToProto(
    stagezero::config::Cgroup::MemoryController *dest) const {
  T(min);
  T(low);
  T(high);
  T(max);
  T(oom_group);
  T(swap_high);
  T(swap_max);
  T(zswap_max);
  T(zswap_writeback);
}

void CgroupMemoryController::FromProto(
    const stagezero::config::Cgroup::MemoryController &src) {
  F(min);
  F(low);
  F(high);
  F(max);
  F(oom_group);
  F(swap_high);
  F(swap_max);
  F(zswap_max);
  F(zswap_writeback);
}

void CgroupCpusetController::ToProto(
    stagezero::config::Cgroup::CpusetController *dest) const {
  T(cpus);
  T(mems);
  T(cpus_exclusive);
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
  F(cpus);
  F(mems);
  F(cpus_exclusive);
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
  T(weight);
  T(max);
}

void CgroupIOController::FromProto(
    const stagezero::config::Cgroup::IOController &src) {
  F(weight);
  F(max);
}

void CgroupPIDController::ToProto(
    stagezero::config::Cgroup::PIDController *dest) const {
  T(max)
}

void CgroupPIDController::FromProto(
    const stagezero::config::Cgroup::PIDController &src) {
  F(max);
}

void CgroupRDMAController::ToProto(
    stagezero::config::Cgroup::RDMAController *dest) const {
  for (auto &device : devices) {
    auto dev = dest->add_device();
    dev->set_name(device.name);
    dev->set_hca_handle(device.hca_handle);

    if (device.hca_object.has_value()) {
      dev->set_hca_object(device.hca_object.value());
    }
  }
}

void CgroupRDMAController::FromProto(
    const stagezero::config::Cgroup::RDMAController &src) {
  for (auto &dev : src.device()) {
    CgroupRDMAController::Device d = {.name = dev.name(),
                                      .hca_handle = dev.hca_handle()};
    if (dev.has_hca_object()) {
      d.hca_object = dev.hca_object();
    }
    devices.push_back(d);
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
  if (pid != nullptr) {
    pid->ToProto(dest->mutable_pid());
  }
  if (rdma != nullptr) {
    rdma->ToProto(dest->mutable_rdma());
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
  if (src.has_pid()) {
    pid = std::make_shared<CgroupPIDController>();
    pid->FromProto(src.pid());
  }
  if (src.has_rdma()) {
    rdma = std::make_shared<CgroupRDMAController>();
    rdma->FromProto(src.rdma());
  }
}
} // namespace adastra
