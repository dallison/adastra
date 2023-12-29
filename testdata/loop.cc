#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "absl/debugging/symbolize.h"
#include "absl/debugging/failure_signal_handler.h"
void Signal(int sig) { printf("Signal %d\n", sig); }

int main(int argc, char** argv) {
    absl::InitializeSymbolizer(argv[0]);

  absl::InstallFailureSignalHandler({
    .use_alternate_stack = false,
  });

  printf("Running\n");
  if (argc == 2 && strcmp(argv[1], "ignore_signal") == 0) {
    printf("Ignoring signals\n");
    signal(SIGINT, Signal);
    signal(SIGTERM, Signal);
  } else {
    printf("SIGINT will terminate\n");
  }

  char* notify = getenv("STAGEZERO_NOTIFY_FD");
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
