#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void Signal(int sig) { printf("Signal %d\n", sig); }

int main(int argc, char **argv) {
  absl::InitializeSymbolizer(argv[0]);

  absl::InstallFailureSignalHandler({
      .use_alternate_stack = false,
  });

  bool ignore_signal = false;
  bool exit_before_notify = false;
  printf("Running\n");
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "ignore_signal") == 0) {
      ignore_signal = true;
    } else if (strcmp(argv[i], "exit_before_notify") == 0) {
      exit_before_notify = true;
    }
  }
  if (ignore_signal) {
    printf("Ignoring signals\n");
    signal(SIGINT, Signal);
    signal(SIGTERM, Signal);
  } else {
    printf("SIGINT will terminate\n");
  }

  if (exit_before_notify) {
    printf("Exiting before notify\n");
    return 0;
  }
  char *notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd = atoi(notify);
    int64_t val = 1;
    (void)write(notify_fd, &val, 8);
  }

  // char buf[256];
  // snprintf(buf, sizeof(buf), "/usr/sbin/lsof -p %d", getpid());
  // system(buf);

  for (int i = 0;; i++) {
    printf("%d loop %d\n", getpid(), i);
    if (!isatty(1)) {
      fflush(stdout);
    }
    usleep(200000);
  }
}
