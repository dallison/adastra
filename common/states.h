
// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <iostream>

namespace adastra {

class Capcom;
class Subsystem;

enum class AdminState {
  kOffline,
  kOnline,
};

enum class OperState {
  kOffline,
  kStartingChildren,
  kConnecting,
  kStartingProcesses,
  kOnline,
  kStoppingProcesses,
  kStoppingChildren,
  kRestarting,
  kRestartingProcesses,
  kBroken,
  kDegraded,
};

inline const char *AdminStateName(AdminState s) {
  switch (s) {
  case AdminState::kOnline:
    return "online";
  case AdminState::kOffline:
    return "offline";
  }
  return "unknown";
}

inline const char *OperStateName(OperState s) {
  switch (s) {
  case OperState::kOnline:
    return "online";
  case OperState::kOffline:
    return "offline";
  case OperState::kConnecting:
    return "connecting";
  case OperState::kStartingChildren:
    return "starting-children";
  case OperState::kStartingProcesses:
    return "starting-processes";
  case OperState::kStoppingProcesses:
    return "stopping-processes";
  case OperState::kStoppingChildren:
    return "stopping-children";
  case OperState::kRestarting:
    return "restarting";
  case OperState::kRestartingProcesses:
    return "restarting-processes";
  case OperState::kBroken:
    return "broken";
  case OperState::kDegraded:
    return "degraded";
  }
  return "unknown";
}

inline std::ostream &operator<<(std::ostream &os, AdminState s) {
  os << AdminStateName(s);
  return os;
}

inline std::ostream &operator<<(std::ostream &os, OperState s) {
  os << OperStateName(s);
  return os;
}

} // namespace adastra
