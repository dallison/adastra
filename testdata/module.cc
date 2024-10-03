#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "stagezero/parameters/parameters.h"
#include "stagezero/symbols.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

void Signal(int sig) { printf("Signal %d\n", sig); }

extern "C" {
extern char **environ;

int Main(const char *enc_syms, int syms_len, int argc, char **argv,
         char **envp) {
  write(1, "foo\n", 4);
  absl::InitializeSymbolizer(argv[0]);

  absl::InstallFailureSignalHandler({
      .use_alternate_stack = false,
  });

  std::cerr << "module main running\n";
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

  std::stringstream symstream;
  symstream.write(enc_syms, syms_len);
  adastra::stagezero::SymbolTable symbols;
  symbols.Decode(symstream);

  std::cerr << "notifying\n";

  // Can do this both with the symbol table or getenv.  Let's do both to
  // make sure it works.
  char *notify_env = getenv("STAGEZERO_NOTIFY_FD");
  adastra::stagezero::Symbol *notify =
      symbols.FindSymbol("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    std::cerr << "notify symbol ok\n";
    if (notify_env == nullptr) {
      fprintf(stderr, "STAGEZERO_NOTIFY_FD is not in the environent\n");
    }
    int notify_fd = atoi(notify->Value().c_str());
    int64_t val = 1;
    (void)write(notify_fd, &val, 8);
  }

  // Print all the parameters.
  stagezero::Parameters params;
  absl::StatusOr<std::vector<std::string>> list = params.ListParameters();
  if (!list.ok()) {
    std::cerr << "Failed to list parameters: " << list.status().message()
              << std::endl;
  } else {
    printf("List of parameters:\n");
    for (const std::string &name : *list) {
      absl::StatusOr<adastra::parameters::Value> value =
          params.GetParameter(name);
      if (!value.ok()) {
        std::cerr << "Failed to get parameter " << name
                  << value.status().message() << std::endl;
        ;
      } else {
        std::cout << name << " = " << *value << std::endl;
      }
    }
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
