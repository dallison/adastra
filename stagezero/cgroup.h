#pragma once

#include <string>
#include "absl/status/status.h"
#include "common/cgroup.h"
#include "toolbelt/logging.h"

namespace adastra::stagezero {

absl::Status CreateCgroup(const Cgroup& cgroup, toolbelt::Logger& logger);
absl::Status RemoveCgroup(const std::string& cgroup, toolbelt::Logger& logger);
absl::Status AddToCgroup(const std::string& proc, const std::string& cgroup, int pid, toolbelt::Logger& logger);
absl::Status FreezeCgroup(const std::string& cgroup);
absl::Status ThawCgroup(const std::string& cgroup);
absl::Status KillCgroup(const std::string& cgroup);

}
