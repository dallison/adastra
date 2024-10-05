#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "stagezero/parameters/parameters.h"
#include <iostream>
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

  printf("Running\n");
  if (argc == 2 && strcmp(argv[1], "ignore_signal") == 0) {
    printf("Ignoring signals\n");
    signal(SIGINT, Signal);
    signal(SIGTERM, Signal);
  } else {
    printf("SIGINT will terminate\n");
  }

  char *notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd = atoi(notify);
    int64_t val = 1;
    (void)write(notify_fd, &val, 8);
  }

  // If there are parameters, they must be /foo/bar and /foo/baz.
  // There might also be local parameters foo/bar and foo/baz.
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
    std::cerr << "Setting parameters\n";
    absl::Status s = params.SetParameter("/foo/bar", "global-foobar");
    if (!s.ok()) {
      std::cerr << "Failed to set parameter: " << s.message() << std::endl;
    } else {
      // Get the parameter and print it.
      absl::StatusOr<adastra::parameters::Value> value =
          params.GetParameter("/foo/bar");
      if (!value.ok()) {
        std::cerr << "Failed to get parameter /foo/bar "
                  << value.status().message() << std::endl;
        ;
      } else {
        std::cout << "/foo/bar = " << *value << std::endl;
      }
      // Try local parameters.
      std::cerr << "Local parameter get:\n";
      value = params.GetParameter("foo/bar");
      if (!value.ok()) {
        std::cerr << "Failed to get parameter foo/bar "
                  << value.status().message() << std::endl;
        ;
      } else {
        std::cout << "foo/bar = " << *value << std::endl;
      }

      // Set a local parameter.
      std::cerr << "Local parameter set:\n";
      s = params.SetParameter("foo/bar", "set-global-foobar");
      if (!s.ok()) {
        std::cerr << "Failed to set local parameter: " << s.message()
                  << std::endl;
      } else {
        // Get the parameter and print it.
        absl::StatusOr<adastra::parameters::Value> value =
            params.GetParameter("foo/bar");
        if (!value.ok()) {
          std::cerr << "Failed to get parameter foo/bar "
                    << value.status().message() << std::endl;
        } else {
          std::cout << "foo/bar = " << *value << std::endl;
        }
      }
      absl::Status s2 = params.DeleteParameter("/foo/baz");
      if (!s2.ok()) {
        std::cerr << "Failed to delete parameter: " << s2.message()
                  << std::endl;
      }
      s2 = params.DeleteParameter("foo/baz");
      if (!s2.ok()) {
        std::cerr << "Failed to delete parameter: " << s2.message()
                  << std::endl;
      }
    }
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
