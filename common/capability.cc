#include "common/capability.h"

#include "absl/strings/str_format.h"

#include <errno.h>
#if defined(__linux__)
#include <sys/prctl.h>
#include <syscall.h>
#endif
#include <sys/stat.h>

namespace adastra {

static struct {
  int num;
  std::string name;
} symbol_table[] = {
#if defined(__linux__)
    {CAP_CHOWN, "CAP_CHOWN"},
    {CAP_DAC_OVERRIDE, "CAP_DAC_OVERRIDE"},
    {CAP_DAC_READ_SEARCH, "CAP_DAC_READ_SEARCH"},
    {CAP_FOWNER, "CAP_FOWNER"},
    {CAP_FSETID, "CAP_FSETID"},
    {CAP_KILL, "CAP_KILL"},
    {CAP_SETGID, "CAP_SETGID"},
    {CAP_SETUID, "CAP_SETUID"},
    {CAP_SETPCAP, "CAP_SETPCAP"},
    {CAP_LINUX_IMMUTABLE, "CAP_LINUX_IMMUTABLE"},
    {CAP_NET_BIND_SERVICE, "CAP_NET_BIND_SERVICE"},
    {CAP_NET_BROADCAST, "CAP_NET_BROADCAST"},
    {CAP_NET_ADMIN, "CAP_NET_ADMIN"},
    {CAP_NET_RAW, "CAP_NET_RAW"},
    {CAP_IPC_LOCK, "CAP_IPC_LOCK"},
    {CAP_IPC_OWNER, "CAP_IPC_OWNER"},
    {CAP_SYS_MODULE, "CAP_SYS_MODULE"},
    {CAP_SYS_RAWIO, "CAP_SYS_RAWIO"},
    {CAP_SYS_CHROOT, "CAP_SYS_CHROOT"},
    {CAP_SYS_PTRACE, "CAP_SYS_PTRACE"},
    {CAP_SYS_PACCT, "CAP_SYS_PACCT"},
    {CAP_SYS_ADMIN, "CAP_SYS_ADMIN"},
    {CAP_SYS_BOOT, "CAP_SYS_BOOT"},
    {CAP_SYS_NICE, "CAP_SYS_NICE"},
    {CAP_SYS_RESOURCE, "CAP_SYS_RESOURCE"},
    {CAP_SYS_TIME, "CAP_SYS_TIME"},
    {CAP_SYS_TTY_CONFIG, "CAP_SYS_TTY_CONFIG"},
    {CAP_MKNOD, "CAP_MKNOD"},
    {CAP_LEASE, "CAP_LEASE"},
    {CAP_AUDIT_WRITE, "CAP_AUDIT_WRITE"},
    {CAP_AUDIT_CONTROL, "CAP_AUDIT_CONTROL"},
    {CAP_SETFCAP, "CAP_SETFCAP"},
    {CAP_MAC_OVERRIDE, "CAP_MAC_OVERRIDE"},
    {CAP_MAC_ADMIN, "CAP_MAC_ADMIN"},
    {CAP_SYSLOG, "CAP_SYSLOG"},
    {CAP_WAKE_ALARM, "CAP_WAKE_ALARM"},
    {CAP_BLOCK_SUSPEND, "CAP_BLOCK_SUSPEND"},
    {CAP_AUDIT_READ, "CAP_AUDIT_READ"},
    {CAP_PERFMON, "CAP_PERFMON"},
    {CAP_BPF, "CAP_BPF"},
    {CAP_CHECKPOINT_RESTORE, "CAP_CHECKPOINT_RESTORE"},
#endif
};

absl::StatusOr<std::string> CapabilitySet::CapabilityToString(int cap) {
  for (size_t i = 0; i < sizeof(symbol_table) / sizeof(symbol_table[0]); i++) {
    if (symbol_table[i].num == cap) {
      return symbol_table[i].name;
    }
  }
  return absl::InternalError(absl::StrFormat("Unknown capability %d", cap));
}

absl::StatusOr<int>
CapabilitySet::CapabilityFromString(const std::string &cap) {
  for (size_t i = 0; i < sizeof(symbol_table) / sizeof(symbol_table[0]); i++) {
    if (symbol_table[i].name == cap) {
      return symbol_table[i].num;
    }
  }
  return absl::InternalError(absl::StrFormat("Unknown capability %s", cap));
}

#if defined(__linux__)

static int capset(cap_user_header_t header, cap_user_data_t data) {
  return syscall(SYS_capset, header, data);
}

static int capget(cap_user_header_t header, const cap_user_data_t data) {
  return syscall(SYS_capget, header, data);
}
#endif

absl::StatusOr<Caps> Caps::Get() {
#if defined(__linux__)
  __user_cap_header_struct hdr = {
      .version = 0,
      .pid = 0,
  };

  // Query supported version.
  capget(&hdr, nullptr);

  __user_cap_data_struct caps[2];
  memset(caps, 0, sizeof(caps));

  if (capget(&hdr, caps) != 0) {
    return absl::InternalError(
        absl::StrFormat("Failed to get capabilities: %s", strerror(errno)));
  }
  Caps c;
  c.effective = uint64_t(caps[0].effective) | uint64_t(caps[1].effective) << 32;
  c.permitted = uint64_t(caps[0].permitted) | uint64_t(caps[1].permitted) << 32;
  c.inheritable = uint64_t(caps[0].inheritable) | uint64_t(caps[1].inheritable)
                                                      << 32;
  return c;
#else
  return absl::InternalError("Caps::Get() is not implemented on this platform");
#endif
}

void Caps::Clear() {
  effective = 0;
  permitted = 0;
  inheritable = 0;
}

void Caps::Modify(int cap, bool add) {
  if (add) {
    effective |= 1ULL << cap;
    permitted |= 1ULL << cap;
    inheritable |= 1ULL << cap;
  } else {
    effective &= ~(1ULL << cap);
    permitted &= ~(1ULL << cap);
    inheritable &= ~(1ULL << cap);
  }
}

absl::Status Caps::Set() const {
#if defined(__linux__)
  __user_cap_header_struct hdr = {
      .version = _LINUX_CAPABILITY_VERSION_3,
      .pid = 0,
  };

  __user_cap_data_struct ucaps[2];
  ucaps[0].effective = effective & 0xFFFFFFFFU;
  ucaps[0].permitted = permitted & 0xFFFFFFFFU;
  ucaps[0].inheritable = inheritable & 0xFFFFFFFFU;
  ucaps[1].effective = (effective >> 32) & 0xFFFFFFFFU;
  ucaps[1].permitted = (permitted >> 32) & 0xFFFFFFFFU;
  ucaps[1].inheritable = (inheritable >> 32) & 0xFFFFFFFFU;
  if (capset(&hdr, ucaps) != 0) {
    return absl::InternalError(
        absl::StrFormat("Failed to set capabilities: %s", strerror(errno)));
  }

  // Add all caps in inheritable to ambient.
  for (int i = 0; i < 64; i++) {
    if (inheritable & (1ULL << i)) {
      int e = prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, i, 0, 0);
      if (e != 0) {
        return absl::InternalError(absl::StrFormat(
            "Failed to set ambient capability: %d: %s", i, strerror(errno)));
      }
    }
  }
  return absl::Status::Ok();
#else
  return absl::InternalError("Caps::Set() is not implemented on this platform");
#endif
}

std::ostream &operator<<(std::ostream &os, const Caps &caps) {
  auto to_string = [](int i) -> std::string {
    auto s = adastra::CapabilitySet::CapabilityToString(i);
    if (!s.ok()) {
      // Print as number prefixed by # if we don't know the name.
      return absl::StrFormat("#%d", i);
    }

    return *s;
  };
  const char *sep = " ";
  os << "Effective:";

  for (int i = 0; i < 64; i++) {
    if (caps.effective & (1ULL << i)) {
      os << sep << to_string(i);
      sep = ", ";
    }
  }
  os << std::endl;

  sep = " ";
  os << "Permitted:";
  for (int i = 0; i < 64; i++) {
    if (caps.permitted & (1ULL << i)) {
      os << sep << to_string(i);
      sep = ", ";
    }
  }

  os << std::endl;
  sep = " ";
  os << "Inheritable:";
  for (int i = 0; i < 64; i++) {
    if (caps.inheritable & (1ULL << i)) {
      os << sep << to_string(i);
      sep = ", ";
    }
  }
  os << std::endl;
  return os;
}

std::string GetCapabilityDetails(const std::filesystem::path &path) {
  std::stringstream details;
  details << "UID: " << getuid() << ", EUID: " << geteuid()
          << ", GID: " << getgid() << ", EGID: " << getegid();
  // Add capabilities to details.
  details << ", Capabilities: ";
  if (auto capabilities = adastra::Caps::Get(); !capabilities.ok()) {
    details << "Error: " << capabilities.status().ToString();
  } else {
    details << *capabilities;
  }
  details << ", File: " << path.string();
  struct stat st;
  if (stat(path.string().c_str(), &st) == 0) {
    details << ", Mode: " << std::oct << st.st_mode << std::dec;
    details << ", Owner: " << st.st_uid;
    details << ", Group: " << st.st_gid;
  } else {
    details << ", Failed to stat: " << strerror(errno);
  }
  return details.str();
}

} // namespace adastra
