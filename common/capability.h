#pragma once
#include "absl/status/status.h"
#include "absl/status/statusor.h"

#include "proto/config.pb.h"

#include <filesystem>
#if defined(__linux__)
#include <linux/capability.h>
#endif
#include <string>

namespace adastra {

// Linux capabilities are specified as strings.  To see what capabilities are
// supported on you system type 'man capabilities'.  If you want to set a
// capability but the name is not available (because it's not in the
// linux/capability.h header) you can use a '#' followed by the capability
// number.  For example, "#37" is CAP_AUDIT_READ.
struct CapabilitySet {
  std::vector<std::string> capabilities;

  void ToProto(stagezero::config::CapabilitySet *dest) const {
    for (auto cap : capabilities) {
      dest->add_capabilities(cap);
    }
  }

  void FromProto(const stagezero::config::CapabilitySet &src) {
    capabilities.clear();
    for (int i = 0; i < src.capabilities_size(); i++) {
      capabilities.push_back(src.capabilities(i));
    }
  }

  // These functions convert a string-based capability name (like
  // "CAP_IPC_OWNER") to and from the number used by the Linux kernel. An
  // unknown capability results in an error.
  static absl::StatusOr<std::string> CapabilityToString(int cap);
  static absl::StatusOr<int> CapabilityFromString(const std::string &cap);
};

// This is a kernel-independent capabilities struct.
class Caps {
public:
  // Get the current capabilities.
  static absl::StatusOr<Caps> Get();

  // Set the capabilities in the kernel.
  absl::Status Set() const;

  // Clear all capabilities, don't set it until Set is called.
  void Clear();

  // Modify a capability, don't set it until Set is called.
  void Modify(int cap, bool add = true);

  // Compare two Caps objects for equality.
  bool operator==(const Caps &other) const {
    return effective == other.effective && permitted == other.permitted &&
           inheritable == other.inheritable;
  }
  // Compare two Caps objects for inequality.
  bool operator!=(const Caps &other) const { return !(*this == other); }

private:
  friend std::ostream &operator<<(std::ostream &os, const Caps &caps);

  uint64_t effective = 0;
  uint64_t permitted = 0;
  uint64_t inheritable = 0;
};

// Print the cap sets to the given output stream.
std::ostream &operator<<(std::ostream &os, const Caps &caps);

std::string GetCapabilityDetails(const std::filesystem::path &path);

} // namespace adastra
