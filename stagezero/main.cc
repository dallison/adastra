// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "coroutine.h"
#include "stagezero/stagezero.h"

#include <iostream>
#include <signal.h>

stagezero::StageZero *g_stagezero;
co::CoroutineScheduler *g_scheduler;

ABSL_FLAG(int, port, 6522, "TCP listening port");
ABSL_FLAG(int, notify_fd, -1, "Notification file descriptor");
ABSL_FLAG(std::string, listen_address, "",
          "IP Address or hostname to listen on");
ABSL_FLAG(bool, silent, false, "Don't log messages to output");
ABSL_FLAG(std::string, logdir, "/tmp",
          "Directory to hold log files from processes");
ABSL_FLAG(std::string, log_level, "info",
          "Log level (verbose, debug, info, warning, error)");

static void Signal(int sig) {
  if (sig == SIGQUIT && g_scheduler != nullptr) {
    g_scheduler->Show();
  }
  if (g_stagezero != nullptr) {
    g_stagezero->Stop();
  }
  if (sig == SIGINT || sig == SIGTERM) {
    return;
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  signal(SIGINT, Signal);
  signal(SIGTERM, Signal);
  signal(SIGHUP, Signal);
  signal(SIGQUIT, Signal);
  signal(SIGPIPE, SIG_IGN);

  co::CoroutineScheduler scheduler;
  g_scheduler = &scheduler;

  std::string listen_addr = absl::GetFlag(FLAGS_listen_address);
  int listen_port = absl::GetFlag(FLAGS_port);
  toolbelt::InetAddress stagezero_addr(
      listen_addr.empty() ? toolbelt::InetAddress::AnyAddress(listen_port)
                          : toolbelt::InetAddress(listen_addr, listen_port));

  stagezero::StageZero stagezero(
      scheduler, stagezero_addr, !absl::GetFlag(FLAGS_silent),
      absl::GetFlag(FLAGS_logdir), absl::GetFlag(FLAGS_log_level),
      absl::GetFlag(FLAGS_notify_fd));
  g_stagezero = &stagezero;
  if (absl::Status status = stagezero.Run(); !status.ok()) {
    std::cerr << "Failed to run StageZero: " << status.ToString() << std::endl;
    exit(1);
  }
}