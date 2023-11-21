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

  toolbelt::InetAddress stagezero_addr("localhost", absl::GetFlag(FLAGS_port));

  stagezero::StageZero stagezero(scheduler, stagezero_addr, -1);
  g_stagezero = &stagezero;
  if (absl::Status status = stagezero.Run(); !status.ok()) {
    std::cerr << "Failed to run StageZero: " << status.ToString() << std::endl;
    exit(1);
  }
}