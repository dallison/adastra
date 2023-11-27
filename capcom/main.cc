// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "capcom/capcom.h"
#include "coroutine.h"

#include <signal.h>
#include <iostream>

stagezero::capcom::Capcom *g_capcom;
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
ABSL_FLAG(std::string, log_file, "/tmp/capcom.pb", "Capcom log file");

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  co::CoroutineScheduler scheduler;
  g_scheduler = &scheduler;

  signal(SIGINT, Signal);
  signal(SIGTERM, Signal);
  signal(SIGQUIT, Signal);
  signal(SIGHUP, Signal);

  toolbelt::InetAddress capcom_addr("localhost", absl::GetFlag(FLAGS_port));

  stagezero::capcom::Capcom capcom(scheduler, capcom_addr,
                                   absl::GetFlag(FLAGS_log_file), -1);
  g_capcom = &capcom;

  if (absl::Status status = capcom.Run(); !status.ok()) {
    std::cerr << "Failed to run Capcom: " << status.ToString() << std::endl;
    exit(1);
  }
}