#include "stagezero/telemetry/telemetry.h"
#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "toolbelt/clock.h"
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  absl::InitializeSymbolizer(argv[0]);

  absl::InstallFailureSignalHandler({
      .use_alternate_stack = false,
  });

  char *notify = getenv("STAGEZERO_NOTIFY_FD");
  if (notify != nullptr) {
    int notify_fd = atoi(notify);
    int64_t val = 1;
    (void)write(notify_fd, &val, 8);
  }

  stagezero::Telemetry telemetry;
  const toolbelt::FileDescriptor &fd = telemetry.GetCommandFD();
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
      // Incoming command.
      absl::StatusOr<std::unique_ptr<stagezero::TelemetryCommand>> cmd =
          telemetry.GetCommand();
      if (!cmd.ok()) {
        std::cerr << "Failed to get command: " << cmd.status().message()
                  << std::endl;
        break;
      }
      switch ((*cmd)->Code()) {
      case stagezero::SystemTelemetry::kShutdownCommand: {
        auto shutdown = dynamic_cast<stagezero::ShutdownCommand *>(cmd->get());
        std::cout << "Shutdown command: exit_code=" << shutdown->exit_code
                  << " timeout_secs=" << shutdown->timeout_secs << std::endl;
        exit(shutdown->exit_code);
        break;
      }
      case stagezero::SystemTelemetry::kDiagnosticsCommand:
        std::cout << "Diagnostic command" << std::endl;
        break;
      default:
        std::cerr << "Unknown command code " << (*cmd)->Code() << std::endl;
        break;
      }
    }
  }
}
