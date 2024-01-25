// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include <iostream>
#include "absl/status/status.h"
#include "stagezero/zygote/zygote_core.h"

int main(int argc, char** argv) {
  adastra::stagezero::ZygoteCore core(argc, argv);
  absl::Status status = core.Run();
  if (!status.ok()) {
    std::cerr << "Failed to run zygote core: " << status.ToString()
              << std::endl;
    exit(1);
  }
}
