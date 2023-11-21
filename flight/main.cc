// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "flight/flight_director.h"
#include <filesystem>
#include <iostream>

ABSL_FLAG(std::string, config_root_dir, "", "Root director for configuration");
ABSL_FLAG(int, port, 6524, "TCP listening port");
ABSL_FLAG(int, capcom_port, 6523, "Capcom client port");

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  co::CoroutineScheduler scheduler;
  toolbelt::InetAddress capcom("localhost", absl::GetFlag(FLAGS_capcom_port));
  toolbelt::InetAddress flight_addr("localhost", absl::GetFlag(FLAGS_port));

  std::string root_dir = absl::GetFlag(FLAGS_config_root_dir);
  if (root_dir.empty()) {
    std::cerr << "Please provide --config_root_dir flag\n";
    exit(1);
  }
  std::filesystem::path root(root_dir);
  if (!std::filesystem::exists(root)) {
    std::cerr << "--config_root_dir directory not found\n";
    exit(1);
  }
  stagezero::flight::FlightDirector flight(scheduler, flight_addr, capcom,
                                           root_dir, -1);
  if (absl::Status status = flight.Run(); !status.ok()) {
    std::cerr << "Failed to run FlightDirector: " << status.ToString()
              << std::endl;
    exit(1);
  }
}
