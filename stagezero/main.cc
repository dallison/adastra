// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "coroutine.h"
#include "stagezero/stagezero.h"

#include <iostream>

ABSL_FLAG(int, port, 6522, "TCP listening port");

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  co::CoroutineScheduler scheduler;
  toolbelt::InetAddress stagezero_addr("localhost", absl::GetFlag(FLAGS_port));

  stagezero::StageZero stagezero(scheduler, stagezero_addr, -1);
  if (absl::Status status = stagezero.Run(); !status.ok()) {
    std::cerr << "Failed to run StageZero: " << status.ToString() << std::endl;
    exit(1);
  }
}