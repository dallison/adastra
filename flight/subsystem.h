// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "common/vars.h"
#include "common/stream.h"
#include <string>
#include <vector>
#include "proto/flight.pb.h"

namespace stagezero::flight {

enum class ProcessType {
  kStatic,
  kZygote,
  kModule,
};

struct Process {
  std::string name;
  std::string description;
  std::vector<Variable> vars;
  std::vector<std::string> args;
  int32_t startup_timeout_secs;
  int32_t sigint_shutdown_timeout_secs;
  int32_t sigterm_shutdown_timeout_secs;
  bool notify;
  std::string compute;
  std::vector<Stream> streams;

  virtual ~Process() = default;
  virtual ProcessType Type() const = 0;
};

struct StaticProcess : public Process {
  std::string executable;
  ProcessType Type() const override { return ProcessType::kStatic; }
};

struct Zygote : public StaticProcess {
  ProcessType Type() const override { return ProcessType::kZygote; }
};

struct Module : public Process {
  std::string dso;
  std::string zygote;
  ProcessType Type() const override { return ProcessType::kModule; }
};

struct Subsystem {
  std::string name;

  std::vector<std::unique_ptr<Process>> processes;
  std::vector<Subsystem *> deps;
  std::vector<Variable> vars;
  std::vector<std::string> args;
};


} // namespace stagezero::flight
