#pragma once

#include <string>
#include <variant>
#include "common/states.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "proto/subsystem_status.pb.h"

namespace stagezero {

struct ProcessStatus {
  std::string name;
  std::string process_id;
  int pid;
  bool running;
};

struct SubsystemStatus {
  std::string subsystem;
  AdminState admin_state;
  OperState oper_state;
  std::vector<ProcessStatus> processes;

  void ToProto(proto::SubsystemStatus *dest) const;
  absl::Status FromProto(const proto::SubsystemStatus &src);
};

}  // namespace stagezero