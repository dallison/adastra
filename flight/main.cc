// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "flight/flight_director.h"
#include <filesystem>
#include <iostream>
#include <signal.h>

stagezero::flight::FlightDirector *g_flight;
co::CoroutineScheduler *g_scheduler;

static void Signal(int sig) {
  if (sig == SIGQUIT && g_scheduler != nullptr) {
    g_scheduler->Show();
  }
  if (g_flight != nullptr) {
    g_flight->Stop();
  }
  if (sig == SIGINT || sig == SIGTERM) {
    return;
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

ABSL_FLAG(std::string, config_root_dir, "", "Root director for configuration");
ABSL_FLAG(int, port, 6524, "TCP listening port");
ABSL_FLAG(int, notify_fd, -1, "Notification file descriptor");
ABSL_FLAG(int, capcom_port, 6523, "Capcom client port");
ABSL_FLAG(bool, silent, false, "Don't log messages to output");
ABSL_FLAG(std::string, listen_address, "",
          "IP Address or hostname to listen on");
ABSL_FLAG(std::string, capcom_address, "localhost",
          "IP Address or hostname of capcom process");
ABSL_FLAG(std::string, log_level, "info",
          "Log level (verbose, debug, info, warning, error)");

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  co::CoroutineScheduler scheduler;
  g_scheduler = &scheduler;

  signal(SIGINT, Signal);
  signal(SIGTERM, Signal);
  signal(SIGQUIT, Signal);
  signal(SIGPIPE, SIG_IGN);

  std::string listen_addr = absl::GetFlag(FLAGS_listen_address);
  int listen_port = absl::GetFlag(FLAGS_port);
  toolbelt::InetAddress flight_addr(
      listen_addr.empty() ? toolbelt::InetAddress::AnyAddress(listen_port)
                          : toolbelt::InetAddress(listen_addr, listen_port));

  std::string capcom_addr = absl::GetFlag(FLAGS_capcom_address);
  toolbelt::InetAddress capcom(capcom_addr, absl::GetFlag(FLAGS_capcom_port));

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
  stagezero::flight::FlightDirector flight(
      scheduler, flight_addr, capcom, root_dir, absl::GetFlag(FLAGS_log_level),
      !absl::GetFlag(FLAGS_silent), absl::GetFlag(FLAGS_notify_fd));
  g_flight = &flight;
  if (absl::Status status = flight.Run(); !status.ok()) {
    std::cerr << "Failed to run FlightDirector: " << status.ToString()
              << std::endl;
    exit(1);
  }
}
