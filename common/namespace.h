// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "proto/config.pb.h"
#include <string>

namespace adastra {

struct Namespace {
  stagezero::config::Namespace_Type type;
  // If/when we support use of the pidfd for cloning a process namespaces, it
  // will go here.

  void ToProto(stagezero::config::Namespace *dest) const;
  void FromProto(const stagezero::config::Namespace &src);
  int CloneType() const;
};
} // namespace adastra
