#include <iostream>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <algorithm>

int main(int argc, char **argv, char** envp) {
  char* notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd = atoi(notify);
    int64_t val = 1;
    (void)write(notify_fd, &val, 8);
  }

  for (int i = 0; i < argc; i++) {
    printf("arg[%d]: %s\n", i, argv[i]);
  }

  // Sort the environment variables to make them checkable.
  std::vector<std::string> env_vars;

  char** v = envp;
  while (*v != nullptr) {
    env_vars.push_back(*v);
    v++;
  }
  std::sort(env_vars.begin(), env_vars.end());
  for (auto& var : env_vars) {
    printf("%s\n", var.c_str());
  }
  printf("DONE\n");
}