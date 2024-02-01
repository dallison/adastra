#include "stagezero/command/s0.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "toolbelt/color.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "toolbelt/table.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>

ABSL_FLAG(std::string, hostname, "localhost",
          "StageZero hostname (or IP address)");
ABSL_FLAG(int, port, 6522, "StageZero listening port");

namespace adastra::stagezero {

StageZeroCommand::StageZeroCommand(toolbelt::InetAddress s0_addr)
    : stagezero_addr_(s0_addr) {
  InitCommands();
}

void StageZeroCommand::Connect() {
  client_ = std::make_unique<Client>();
  if (absl::Status status = client_->Init(stagezero_addr_, "StageZeroCommand");
      !status.ok()) {
    std::cerr << "Can't connect to StageZero at address "
              << stagezero_addr_.ToString() << ": " << status.ToString()
              << std::endl;
    exit(1);
  }
}

void StageZeroCommand::AddCommand(std::unique_ptr<Command> cmd) {
  std::string root = cmd->Root();
  commands_.insert(std::make_pair(std::move(root), std::move(cmd)));
}

void StageZeroCommand::InitCommands() {
  AddCommand(std::make_unique<HelpCommand>());
  AddCommand(std::make_unique<LaunchStaticCommand>());
  AddCommand(std::make_unique<StopCommand>());
}

std::shared_ptr<adastra::stagezero::control::Event>
StageZeroCommand::WaitForEvent(Client *client) {
  absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
      client->WaitForEvent();
  if (!e.ok()) {
    std::cerr << "Error receiving event: " << e.status() << std::endl;
    exit(1);
  }
  return *e;
}

absl::Status HelpCommand::Execute(Client *client, int argc, char **argv) const {
  std::cout << "Talk to StageZero\n";
  std::cout << "s0 launch-static <name> <executable> [args] - launch static "
               "process\n";
  std::cout << "s0 stop <process_id> - stop a process\n";
  return absl::OkStatus();
}

absl::Status LaunchStaticCommand::Execute(Client *client, int argc,
                                          char **argv) const {
  std::string name;
  std::string executable;
  std::vector<std::string> args;
  ProcessOptions opts = {.sigint_shutdown_timeout_secs = 0, .detached = true};

  bool process_flags = true;
  for (int i = 2; i < argc; i++) {
    if (process_flags && argv[i][0] == '-') {
      std::string arg = argv[i];
      if (arg == "--") {
        process_flags = !process_flags;
        continue;
      }
      // Options in here.
      if (arg == "-notify") {
        opts.notify = true;
      } else if (arg == "-cgroup") {
        i++;
        if (i == argc) {
          std::cerr << "-cgroup needs a value\n";
          exit(1);
        }
        opts.cgroup = argv[i];
      } else {
        std::cerr << "Unknown launch-static option: " << argv[i] << std::endl;
        exit(1);
      }
    } else {
      if (name.empty()) {
        name = argv[i];
      } else if (executable.empty()) {
        executable = argv[i];
      } else {
        opts.args.push_back(argv[i]);
      }
    }
  }
  if (executable.empty()) {
    return absl::InternalError(
        "usage: s0 launch-static <name> <executable> [args]");
  }

  absl::StatusOr<std::pair<std::string, int>> status =
      client->LaunchStaticProcess(name, executable, opts);
  if (!status.ok()) {
    return status.status();
  }
  std::cout << "process is " << status->first << " with PID " << status->second
            << std::endl;
  std::shared_ptr<adastra::stagezero::control::Event> event =
      StageZeroCommand::WaitForEvent(client);

  switch (event->event_case()) {
  case adastra::stagezero::control::Event::kStart:
    std::cout << "Process started OK\n";
    break;
  case adastra::stagezero::control::Event::kStop:
    std::cout << "Process failed to start\n";
    // Show reason.
    break;
  default:
    break;
  }
  return absl::OkStatus();
}

absl::Status StopCommand::Execute(Client *client, int argc, char **argv) const {
  if (argc < 3) {
    std::cerr << "stop <process-id>\n";
    exit(1);
  }
  std::string process_id = argv[2];
  std::cout << "stopping process " << process_id << std::endl;
  if (absl::Status status = client->StopProcess(process_id); !status.ok()) {
    return status;
  }
  std::shared_ptr<adastra::stagezero::control::Event> event =
      StageZeroCommand::WaitForEvent(client);

  switch (event->event_case()) {
  case adastra::stagezero::control::Event::kStart:
    break;
  case adastra::stagezero::control::Event::kStop:
    std::cout << "Process stopped OK\n";
    // Show reason.
    break;
  default:
    break;
  }
  return absl::OkStatus();
}

void StageZeroCommand::Run(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: s0 <command...>\n";
    exit(1);
  }
  std::string root = argv[1];
  auto it = commands_.find(root);
  if (it == commands_.end()) {
    std::cerr << "unknown s0 command " << root << std::endl;
    exit(1);
  }
  bool connect_live = true;
  if (it->first == "help") {
    connect_live = false;
  }
  if (connect_live) {

    Connect();
  }
  if (absl::Status status = it->second->Execute(client_.get(), argc, argv);
      !status.ok()) {
    std::cerr << status.ToString() << std::endl;
    exit(1);
  }
}

} // namespace adastra::stagezero

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  toolbelt::InetAddress s0_addr(absl::GetFlag(FLAGS_hostname),
                                absl::GetFlag(FLAGS_port));
  adastra::stagezero::StageZeroCommand s0(s0_addr);

  s0.Run(argc, argv);
}
