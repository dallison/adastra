// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

// Reports what this process inherited from its launcher.  Prints one line:
//   <num sockets> <num eventfds> <1 if SIGPIPE is ignored, else 0>

#include <dirent.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main() {
  DIR *dir = opendir("/proc/self/fd");
  if (dir == nullptr) {
    return 1;
  }
  int num_sockets = 0;
  int num_eventfds = 0;
  while (struct dirent *entry = readdir(dir)) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
    char target[PATH_MAX];
    ssize_t len = readlink(path, target, sizeof(target) - 1);
    if (len <= 0) {
      continue;
    }
    target[len] = '\0';
    if (strncmp(target, "socket:[", 8) == 0) {
      num_sockets++;
    } else if (strcmp(target, "anon_inode:[eventfd]") == 0) {
      num_eventfds++;
    }
  }
  closedir(dir);

  struct sigaction action;
  if (sigaction(SIGPIPE, nullptr, &action) == -1) {
    return 1;
  }
  int sigpipe_ignored = action.sa_handler == SIG_IGN ? 1 : 0;

  printf("%d %d %d\n", num_sockets, num_eventfds, sigpipe_ignored);
  return 0;
}
