// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "coroutine.h"
#include "stagezero/stagezero.h"

#include <signal.h>
#include <iostream>

stagezero::StageZero *g_stagezero;
co::CoroutineScheduler *g_scheduler;

ABSL_FLAG(int, port, 6522, "TCP listening port");
ABSL_FLAG(std::string, listen_address, "",
          "IP Address or hostname to listen on");
ABSL_FLAG(bool, silent, false, "Don't log messages to output");

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

  co::CoroutineScheduler scheduler;
  g_scheduler = &scheduler;

  std::string listen_addr = absl::GetFlag(FLAGS_listen_address);
  int listen_port = absl::GetFlag(FLAGS_port);
  toolbelt::InetAddress stagezero_addr(
      listen_addr.empty() ? toolbelt::InetAddress::AnyAddress(listen_port)
                          : toolbelt::InetAddress(listen_addr, listen_port));

  stagezero::StageZero stagezero(scheduler, stagezero_addr, !absl::GetFlag(FLAGS_silent), -1);
  g_stagezero = &stagezero;
  if (absl::Status status = stagezero.Run(); !status.ok()) {
    std::cerr << "Failed to run StageZero: " << status.ToString() << std::endl;
    exit(1);
  }
}