// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <string>

namespace stagezero {

struct Variable {
  std::string name;
  std::string value;
  bool exported = false;
};

}  // namespace stagezero