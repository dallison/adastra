#include "common/cgroup.h"
#include <fstream>
#include "common/capability.h"

// Parts of this file are provided by Cruise LLC.

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

static stagezero::config::SubtreeControl
SubtreeControlToProto(SubtreeControl control) {
  switch (control) {
  case SubtreeControl::kDefault:
    return stagezero::config::SubtreeControl::SUBTREE_DEFAULT;
  case SubtreeControl::kEnable:
    return stagezero::config::SubtreeControl::SUBTREE_ENABLE;
  case SubtreeControl::kDisable:
    return stagezero::config::SubtreeControl::SUBTREE_DISABLE;
  }
}

static SubtreeControl
SubtreeControlFromProto(stagezero::config::SubtreeControl control) {
  switch (control) {
  default:
  case stagezero::config::SubtreeControl::SUBTREE_DEFAULT:
    return SubtreeControl::kDefault;
  case stagezero::config::SubtreeControl::SUBTREE_ENABLE:
    return SubtreeControl::kEnable;
  case stagezero::config::SubtreeControl::SUBTREE_DISABLE:
    return SubtreeControl::kDisable;
  }
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
  dest->set_subtree_control(SubtreeControlToProto(subtree_control));
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
  subtree_control = SubtreeControlFromProto(src.subtree_control());
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
  dest->set_subtree_control(SubtreeControlToProto(subtree_control));
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
  subtree_control = SubtreeControlFromProto(src.subtree_control());
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
  dest->set_subtree_control(SubtreeControlToProto(subtree_control));
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
  subtree_control = SubtreeControlFromProto(src.subtree_control());
}

void CgroupIOController::ToProto(
    stagezero::config::Cgroup::IOController *dest) const {
  T(weight);
  T(max);
  dest->set_subtree_control(SubtreeControlToProto(subtree_control));
}

void CgroupIOController::FromProto(
    const stagezero::config::Cgroup::IOController &src) {
  F(weight);
  F(max);
  subtree_control = SubtreeControlFromProto(src.subtree_control());
}

void CgroupPIDController::ToProto(
    stagezero::config::Cgroup::PIDController *dest) const {
  T(max);
  dest->set_subtree_control(SubtreeControlToProto(subtree_control));
}

void CgroupPIDController::FromProto(
    const stagezero::config::Cgroup::PIDController &src) {
  F(max);
  subtree_control = SubtreeControlFromProto(src.subtree_control());
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
  dest->set_subtree_control(SubtreeControlToProto(subtree_control));
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
  subtree_control = SubtreeControlFromProto(src.subtree_control());
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
  if (pids != nullptr) {
    pids->ToProto(dest->mutable_pid());
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
    pids = std::make_shared<CgroupPIDController>();
    pids->FromProto(src.pid());
  }
  if (src.has_rdma()) {
    rdma = std::make_shared<CgroupRDMAController>();
    rdma->FromProto(src.rdma());
  }
}

absl::StatusOr<SubtreeControlSettings>
SubtreeControlSettings::FromFile(const std::filesystem::path &file) {
  if (!std::filesystem::exists(file)) {
    return absl::InternalError(absl::StrFormat("No such file '%s'", file));
  } else if (file.extension() != ".subtree_control") {
    return absl::InternalError(
        absl::StrFormat("Subtree control file '%s' does not end with "
                        "'.subtree_control' extension",
                        file));
  }

  // Read control file
  std::ifstream ifs(file);
  if (!ifs.is_open()) {
    return absl::InternalError(absl::StrFormat("Failed to open file '%s': %s", file,
                                           GetCapabilityDetails(file)));
  }

  std::stringstream iss;
  std::string line;
  while (std::getline(ifs, line)) {
    iss << line;
  }
  std::string content(iss.str());
  absl::StripAsciiWhitespace(&content);

  // Parse the file into individual words.
  // We cannot simply search for substrings like 'cpu' when 'cpuset' is also
  // valid
  const auto tokens = Split(content, ' ');
  const absl::flat_hash_set<std::string> entries{tokens.cbegin(),
                                                 tokens.cend()};

  auto parseSubtreeKeyword = [&entries](const auto &keyword) {
    // Once the OS processes the keyword, the file no longer contains + or -
    // However, we support keyword, +keyword, or -keyword for testing
    if (entries.contains(keyword)) {
      return SubtreeControl::kEnable;
    } else if (entries.contains(absl::StrFormat("+%s", keyword))) {
      return SubtreeControl::kEnable;
    } else if (entries.contains(absl::StrFormat("-%s", keyword))) {
      return SubtreeControl::kDisable;
    }
    return SubtreeControl::kDefault;
  };

  SubtreeControlSettings settings;
  settings.cpu = parseSubtreeKeyword("cpu");
  settings.cpuset = parseSubtreeKeyword("cpuset");
  settings.memory = parseSubtreeKeyword("memory");
  settings.io = parseSubtreeKeyword("io");
  settings.pids = parseSubtreeKeyword("pids");
  settings.rdma = parseSubtreeKeyword("rdma");

  return settings;
}

SubtreeControlSettings SubtreeControlSettings::ForCgroup(const Cgroup &cgroup) {
  SubtreeControlSettings settings;
  if (cgroup.cpu != nullptr) {
    settings.cpu = cgroup.cpu->subtree_control;
  }
  if (cgroup.cpuset != nullptr) {
    settings.cpuset = cgroup.cpuset->subtree_control;
  }
  if (cgroup.memory != nullptr) {
    settings.memory = cgroup.memory->subtree_control;
  }
  if (cgroup.io != nullptr) {
    settings.io = cgroup.io->subtree_control;
  }
  if (cgroup.pids != nullptr) {
    settings.pids = cgroup.pids->subtree_control;
  }
  if (cgroup.rdma != nullptr) {
    settings.rdma = cgroup.rdma->subtree_control;
  }
  return settings;
}

std::string SubtreeControlSettings::ToString() const {
  std::stringstream value;
  value << *this;
  return value.str();
}

absl::Status SubtreeControlSettings::Write(
    const std::filesystem::path &file) const {
  const auto content = ToString();
  if (!content.empty()) {
    std::ofstream out(file);
    if (!out) {
      return absl::InternalError(
          absl::StrFormat("Failed to write to file %s", file));
    }
    out << content << std::endl;
  }
  return absl::OkStatus();
}

bool operator==(const SubtreeControlSettings &lhs,
                const SubtreeControlSettings &rhs) {
  return lhs.cpu == rhs.cpu && lhs.cpuset == rhs.cpuset &&
         lhs.memory == rhs.memory && lhs.io == rhs.io && lhs.pids == rhs.pids &&
         lhs.rdma == rhs.rdma;
}

bool operator!=(const SubtreeControlSettings &lhs,
                const SubtreeControlSettings &rhs) {
  return !(lhs == rhs);
}

std::ostream &operator<<(std::ostream &os,
                         const SubtreeControlSettings &settings) {
  std::stringstream subtree_control;
  HandleSubtreeControl(subtree_control, "cpu", settings.cpu);
  HandleSubtreeControl(subtree_control, "cpuset", settings.cpuset);
  HandleSubtreeControl(subtree_control, "memory", settings.memory);
  HandleSubtreeControl(subtree_control, "io", settings.io);
  HandleSubtreeControl(subtree_control, "pids", settings.pids);
  HandleSubtreeControl(subtree_control, "rdma", settings.rdma);
  os << subtree_control.str();
  return os;
}

void HandleSubtreeControl(std::stringstream &subtree_control,
                          const std::string &controller,
                          SubtreeControl control) {
  switch (control) {
  case SubtreeControl::kEnable:
    EnableSubtreeControl(subtree_control, controller);
    break;
  case SubtreeControl::kDisable:
    DisableSubtreeControl(subtree_control, controller);
    break;
  case SubtreeControl::kDefault:
    break;
  }
}

void EnableSubtreeControl(std::stringstream &subtree_control,
                          const std::string &controller) {
  if (!subtree_control.str().empty()) {
    subtree_control << " ";
  }
  subtree_control << "+" << controller;
}

void DisableSubtreeControl(std::stringstream &subtree_control,
                           const std::string &controller) {
  if (!subtree_control.str().empty()) {
    subtree_control << " ";
  }
  subtree_control << "-" << controller;
}

std::vector<std::string>
SubtreeControlSettings::GetExpectedSubtreeFiles() const {
  const absl::flat_hash_map<SubtreeCategory, std::vector<std::string>>
      kExpectedFilesByCategory{
          {SubtreeCategory::kDefault,
           {"cgroup.controllers", "cgroup.events", "cgroup.freeze",
            "cgroup.kill", "cgroup.max.depth", "cgroup.max.descendants",
            "cgroup.procs", "cgroup.stat", "cgroup.subtree_control",
            "cgroup.threads", "cgroup.type"}},
          {SubtreeCategory::kCpu,
           {"cpu.idle", "cpu.stat", "cpu.weight", "cpu.weight.nice"}},
          {SubtreeCategory::kCpuset,
           {"cpuset.cpus", "cpuset.cpus.effective", "cpuset.cpus.partition",
            "cpuset.mems", "cpuset.mems.effective"}},
          {SubtreeCategory::kMemory,
           {"memory.current", "memory.events", "memory.events.local",
            "memory.high", "memory.low", "memory.max", "memory.min",
            "memory.numa_stat", "memory.oom.group", "memory.stat",
            "memory.swap.current", "memory.swap.events", "memory.swap.high",
            "memory.swap.max"}},
          {SubtreeCategory::kIo, {"io.bfq.weight", "io.pressure", "io.stat"}},
          {SubtreeCategory::kPids, {"pids.current", "pids.events", "pids.max"}},
          {SubtreeCategory::kRdma, {"rdma.current", "rdma.max"}}};

  auto insert_expected_files =
      [&kExpectedFilesByCategory](const SubtreeCategory category,
                                  const SubtreeControl value,
                                  std::vector<std::string> &files) {
        if (value == SubtreeControl::kEnable) {
          for (const auto &file : kExpectedFilesByCategory.at(category)) {
            files.emplace_back(file);
          }
        }
      };

  std::vector<std::string> files;
  insert_expected_files(SubtreeCategory::kDefault, SubtreeControl::kEnable,
                        files);
  insert_expected_files(SubtreeCategory::kCpu, cpu, files);
  insert_expected_files(SubtreeCategory::kCpuset, cpuset, files);
  insert_expected_files(SubtreeCategory::kMemory, memory, files);
  insert_expected_files(SubtreeCategory::kIo, io, files);
  insert_expected_files(SubtreeCategory::kPids, pids, files);
  insert_expected_files(SubtreeCategory::kRdma, rdma, files);
  return files;
}

std::vector<std::string> Split(const std::string &text, char delimiter) {
  std::vector<std::string> tokens;
  std::stringstream ss(text);
  std::string token;

  while (std::getline(ss, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

} // namespace adastra
