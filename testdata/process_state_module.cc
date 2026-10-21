// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

// A virtual process module that prints the credentials it runs with.

#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

extern "C" {

int Main(const char *enc_syms, int syms_len, int argc, char **argv,
         char **envp) {
  printf("uid: %d gid: %d dumpable: %d\n", static_cast<int>(geteuid()),
         static_cast<int>(getegid()), prctl(PR_GET_DUMPABLE, 0, 0, 0, 0));

  unsigned long long cap_eff = 0;
  if (FILE *status = fopen("/proc/self/status", "r"); status != nullptr) {
    char line[256];
    while (fgets(line, sizeof(line), status) != nullptr) {
      if (sscanf(line, "CapEff: %llx", &cap_eff) == 1) {
        break;
      }
    }
    fclose(status);
  }
  printf("cap_eff: %llx\n", cap_eff);
  printf("done\n");
  fflush(stdout);
  return 0;
}
}
