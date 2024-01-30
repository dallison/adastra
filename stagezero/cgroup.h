#pragma once

#include <string>
#include "absl/status/status.h"
#include "common/cgroup.h"

namespace adastra::stagezero {

absl::Status CreateCgroup(const Cgroup& cgroup);
absl::Status RemoveCgroup(const std::string& cgroup);
absl::Status AddToCgroup(const std::string& proc, const std::string& cgroup, int pid);
absl::Status RemoveFromCgroup(const std::string& proc, const std::string& cgroup, int pid);

}
