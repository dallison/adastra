#pragma once

#include "absl/status/status.h"
#include "common/cgroup.h"
#include "toolbelt/logging.h"
#include <string>
#include "coroutine.h"

namespace adastra::stagezero {

absl::Status CreateCgroup(const Cgroup &cgroup,
                          const std::string &cgroup_root_dir,
                          toolbelt::Logger &logger, co::Coroutine *c = nullptr);
absl::Status RemoveCgroup(const std::string &cgroup,
                          const std::string &cgroup_root_dir,
                          toolbelt::Logger &logger);
absl::Status AddToCgroup(const std::string &proc, const std::string &cgroup,
                         int pid, const std::string &cgroup_root_dir,
                         toolbelt::Logger &logger);
absl::Status FreezeCgroup(const std::string &cgroup,
                          const std::string &cgroup_root_dir,
                          toolbelt::Logger &logger);
absl::Status ThawCgroup(const std::string &cgroup,
                        const std::string &cgroup_root_dir,
                        toolbelt::Logger &logger);
absl::Status KillCgroup(const std::string &cgroup,
                        const std::string &cgroup_root_dir,
                        toolbelt::Logger &logger);

} // namespace adastra::stagezero
