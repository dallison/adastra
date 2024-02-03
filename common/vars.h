// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <string>

namespace adastra {

struct Variable {
  std::string name;
  std::string value;
  bool exported = false;
};

}  // namespace adastra
