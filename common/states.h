
#pragma once

namespace stagezero::capcom {

class Capcom;
class Subsystem;

enum class AdminState {
  kOffline,
  kOnline,
};

enum class OperState {
  kOffline,
  kStartingChildren,
  kStartingProcesses,
  kOnline,
  kStoppingProcesses,
  kStoppingChildren,
  kRestarting,
  kBroken,
};

inline const char *AdminStateName(AdminState s) {
  switch (s) {
  case AdminState::kOnline:
    return "kOnline";
  case AdminState::kOffline:
    return "kOffline";
  }
}

inline const char *OperStateName(OperState s) {
  switch (s) {
  case OperState::kOnline:
    return "kOnline";
  case OperState::kOffline:
    return "kOffline";
  case OperState::kStartingChildren:
    return "kStartingShildren";
  case OperState::kStartingProcesses:
    return "kStartingProcesses";
  case OperState::kStoppingProcesses:
    return "kStoppingProcesses";
  case OperState::kStoppingChildren:
    return "kStoppingChildren";
  case OperState::kRestarting:
    return "kRestarting";  
   case OperState::kBroken:
    return "kBroken";  
   }
}

} // namespace stagezero::capcom
