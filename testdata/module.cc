#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>
#include "stagezero/symbols.h"
#include "absl/debugging/symbolize.h"
#include "absl/debugging/failure_signal_handler.h"

void Signal(int sig) { printf("Signal %d\n", sig); }

extern "C" {
extern char **environ;

int Main(stagezero::SymbolTable&& symbols, int argc, char **argv, char **envp) {
  absl::InitializeSymbolizer(argv[0]);

  absl::FailureSignalHandlerOptions options;
  absl::InstallFailureSignalHandler(options);

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

  std::cerr << "notifying\n";

  // Can do this both with the symbol table or getenv.  Let's do both to
  // make sure it works.
  char* notify_env = getenv("STAGEZERO_NOTIFY_FD");
  stagezero::Symbol* notify = symbols.FindSymbol("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    std::cerr << "notify symbol ok\n";
    if (notify_env == nullptr) {
      fprintf(stderr, "STAGEZERO_NOTIFY_FD is not in the environent\n");
    }
    int notify_fd = atoi(notify->Value().c_str());
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