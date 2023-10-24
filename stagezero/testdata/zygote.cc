#include "stagezero/zygote/zygote_core.h"
#include "absl/status/status.h"
#include <iostream>

int main(int argc, char** argv) {
  stagezero::ZygoteCore core(argc, argv);
  absl::Status status = core.Run();
  if (!status.ok()) {
    std::cerr << "Failed to run zygote core: " << status.ToString() << std::endl;
    exit(1);
  }
}