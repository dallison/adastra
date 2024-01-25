// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "fido/fido.h"

ABSL_FLAG(std::string, hostname, "localhost",
          "Flight Director hostname (or IP address)");
ABSL_FLAG(int, port, 6524, "Flight Director listening port");
ABSL_FLAG(std::string, subspace_socket, "/tmp/subspace",
          "Subspace server socket name");

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  toolbelt::InetAddress flight_addr(absl::GetFlag(FLAGS_hostname),
                                    absl::GetFlag(FLAGS_port));

  fido::FDOApplication app(flight_addr, absl::GetFlag(FLAGS_subspace_socket));
  app.Run();
}