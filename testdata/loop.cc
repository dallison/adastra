#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "stagezero/parameters/parameters.h"
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
  bool parameter_events = false;
  printf("Running\n");
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "ignore_signal") == 0) {
      ignore_signal = true;
    } else if (strcmp(argv[i], "parameter_events") == 0) {
      parameter_events = true;
    }
  }
  if (ignore_signal) {
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
  stagezero::Parameters params(parameter_events);
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
      s = params.SetParameter("foo/bar", "set-local-foobar");
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

  if (parameter_events) {
    const toolbelt::FileDescriptor &fd = params.GetEventFD();
    struct pollfd pfd = {
        .fd = fd.Fd(), .events = POLLIN,
    };
    for (;;) {
      int ret = poll(&pfd, 1, 0);
      if (ret < 0) {
        perror("poll");
        break;
      }
      if (pfd.revents & POLLIN) {
        std::unique_ptr<adastra::parameters::ParameterEvent> event =
            params.GetEvent();
        std::cerr << "Event: " << std::endl;
        switch (event->type) {
        case adastra::parameters::ParameterEvent::Type::kUpdate: {
          adastra::parameters::ParameterUpdateEvent *update =
              static_cast<adastra::parameters::ParameterUpdateEvent *>(
                  event.get());

          std::cerr << "Update: " << update->name << " = " << update->value
                    << std::endl;
          break;
        }
        case adastra::parameters::ParameterEvent::Type::kDelete: {
          adastra::parameters::ParameterDeleteEvent *del =
              static_cast<adastra::parameters::ParameterDeleteEvent *>(
                  event.get());
          std::cerr << "Delete: " << del->name << std::endl;
          break;
        }
        }
        continue;
      }
      break;
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
