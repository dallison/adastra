#pragma once

#include <string>

namespace stagezero {
  
struct Variable {
  std::string name;
  std::string value;
  bool exported = false;
};


}