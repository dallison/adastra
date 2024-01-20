// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <string>
#include <variant>
#include "common/states.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "proto/subsystem_status.pb.h"

namespace stagezero {

enum class ProcessType {
  kStatic,
  kZygote,
  kVirtual,
};

struct ProcessStatus {
  std::string name;
  std::string process_id;
  int pid;
  bool running;
  ProcessType type;
  std::string compute;
  std::string subsystem;
  int alarm_count;
};

struct SubsystemStatus {
  std::string subsystem;
  AdminState admin_state;
  OperState oper_state;
  std::vector<ProcessStatus> processes;
  int alarm_count;
  int restart_count;
  void ToProto(proto::SubsystemStatus *dest) const;
  absl::Status FromProto(const proto::SubsystemStatus &src);
};

}  // namespace stagezero