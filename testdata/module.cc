#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>

void Signal(int sig) { printf("Signal %d\n", sig); }

extern "C" {
extern char **environ;

int Main(int argc, char **argv, char **envp) {
  for (int i = 0; i < argc; i++) {
    printf("arg[%d]: %s\n", i, argv[i]);
  }

  if (argc == 2 && strcmp(argv[1], "ignore_signal") == 0) {
    printf("Ignoring signals\n");
    signal(SIGINT, Signal);
    signal(SIGTERM, Signal);
  } else {
    printf("SIGINT will terminate\n");
  }
  if (!isatty(1)) {
    fflush(stdout);
  }

  char *notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd = atoi(notify);
    int64_t val = 1;
    (void)write(notify_fd, &val, 8);
  }

  char **v = envp;
  printf("Env from arg\n");
  while (*v != nullptr) {
    printf("%s\n", *v);
    v++;
  }
  v = environ;
  printf("Env from environ\n");
  while (*v != nullptr) {
    printf("%s\n", *v);
    v++;
  }

  if (!isatty(1)) {
    fflush(stdout);
  }

  for (int i = 0;; i++) {
    printf("Module loop %d\n", i);
    if (!isatty(1)) {
      fflush(stdout);
    }
    usleep(200000);
  }
}
}