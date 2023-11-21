// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "capcom/capcom.h"
#include "coroutine.h"

#include <iostream>

ABSL_FLAG(int, port, 6523, "TCP listening port");

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  co::CoroutineScheduler scheduler;
  toolbelt::InetAddress capcom_addr("localhost", absl::GetFlag(FLAGS_port));

  stagezero::capcom::Capcom capcom(scheduler, capcom_addr, -1);
  if (absl::Status status = capcom.Run(); !status.ok()) {
    std::cerr << "Failed to run Capcom: " << status.ToString() << std::endl;
    exit(1);
  }
}