// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "capcom/capcom.h"
#include "coroutine.h"

#include <iostream>
#include <signal.h>

adastra::capcom::Capcom *g_capcom;
co::CoroutineScheduler *g_scheduler;

static void Signal(int sig) {
  if (sig == SIGQUIT && g_scheduler != nullptr) {
    g_scheduler->Show();
  }
  if (g_capcom != nullptr) {
    g_capcom->Stop();
  }
  if (sig == SIGINT || sig == SIGTERM) {
    return;
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

ABSL_FLAG(int, port, 6523, "TCP listening port");
ABSL_FLAG(int, notify_fd, -1, "Notification file descriptor");
ABSL_FLAG(int, local_stagezero_port, 6522, "Local StageZero listening port");
ABSL_FLAG(std::string, log_file, "", "Capcom log file");
ABSL_FLAG(std::string, listen_address, "",
          "IP Address or hostname to listen on");
ABSL_FLAG(bool, silent, false, "Don't log messages to output");
ABSL_FLAG(bool, test_mode, false, "Exit if any subsystem fails (no restarts)");
ABSL_FLAG(std::string, log_level, "debug",
          "Log level (verbose, debug, info, warning, error)");
ABSL_FLAG(bool, log_process_output, true,
          "Log process output to the capcom logger (default: true)");

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  co::CoroutineScheduler scheduler;
  g_scheduler = &scheduler;

  signal(SIGINT, Signal);
  signal(SIGTERM, Signal);
  signal(SIGQUIT, Signal);
  signal(SIGHUP, Signal);
  signal(SIGPIPE, SIG_IGN);

  std::string listen_addr = absl::GetFlag(FLAGS_listen_address);
  int listen_port = absl::GetFlag(FLAGS_port);
  toolbelt::InetAddress capcom_addr(
      listen_addr.empty() ? toolbelt::InetAddress::AnyAddress(listen_port)
                          : toolbelt::InetAddress(listen_addr, listen_port));

  auto capcom = std::make_unique<adastra::capcom::Capcom>(
      scheduler, capcom_addr, !absl::GetFlag(FLAGS_silent),
      absl::GetFlag(FLAGS_local_stagezero_port), absl::GetFlag(FLAGS_log_file),
      absl::GetFlag(FLAGS_log_level), absl::GetFlag(FLAGS_test_mode),
      absl::GetFlag(FLAGS_notify_fd), absl::GetFlag(FLAGS_log_process_output));
  g_capcom = capcom.get();

  if (absl::Status status = capcom->Run(); !status.ok()) {
    std::cerr << "Failed to run Capcom: " << status.ToString() << std::endl;
    exit(1);
  }
}
