#include "stagezero/cgroup.h"

#include <iostream>

namespace adastra::stagezero {

absl::Status CreateCgroup(const Cgroup &cgroup) {
#if defined(__linux__)
  // TODO
  return absl::OkStatus();
#else
  std::cout << "Cgroups are not supported on this OS; creation of cgroup '"
            << cgroup.name << "' ignored" << std::endl;
  return absl::OkStatus();
#endif
}

absl::Status RemoveCgroup(const std::string &cgroup) {
#if defined(__linux__)
  // TODO
  return absl::OkStatus();
#else
  std::cout << "Cgroups are not supported on this OS; removal of cgroup '"
            << cgroup << "' ignored" << std::endl;
  return absl::OkStatus();
#endif
}

absl::Status AddToCgroup(const std::string &proc, const std::string &cgroup,
                         int pid) {
#if defined(__linux__)
  // TODO
  return absl::OkStatus();
#else
  std::cout << "Cgroups are not supported on this OS; addition of process "
            << proc << " with pid " << pid << " to cgroup '" << cgroup
            << "' ignored" << std::endl;
  return absl::OkStatus();
#endif
}

absl::Status RemoveFromCgroup(const std::string &proc,
                              const std::string &cgroup, int pid) {
#if defined(__linux__)
  // TODO
  return absl::OkStatus();
#else
  std::cout << "Cgroups are not supported on this OS; removal of process "
            << proc << " with pid " << pid << " from cgroup '" << cgroup
            << "' ignored" << std::endl;
  return absl::OkStatus();
#endif
}

} // namespace adastra::stagezero
