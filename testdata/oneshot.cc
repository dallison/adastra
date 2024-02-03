#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  printf("oneshot running\n");

  char *notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd = atoi(notify);
    int64_t val = 1;
    (void)write(notify_fd, &val, 8);
  }

  sleep(1);
  if (argc == 2) {
    if (strcmp(argv[1], "--fail") == 0) {
      exit(1);
    } else if (strcmp(argv[1], "--signal") == 0) {
      printf("sending signal\n");
      kill(getpid(), SIGINT);
    }
  }
  exit(0);
}
