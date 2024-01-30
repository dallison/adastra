#pragma once

#include "proto/config.pb.h"
#include <cstdint>
#include <string>

namespace adastra {

struct Cgroup {
  std::string name;
  std::string cpuset;
  uint32_t cpushare;
  uint64_t memory;

  void ToProto(stagezero::config::Cgroup *dest) const {
    dest->set_name(name);
    dest->set_cpuset(cpuset);
    dest->set_cpushare(cpushare);
    dest->set_memory(memory);
  }

  void FromProto(const stagezero::config::Cgroup &src) {
    name = src.name();
    cpuset = src.cpuset();
    cpushare = src.cpushare();
    memory = src.memory();
  }
};

} // namespace adastra
