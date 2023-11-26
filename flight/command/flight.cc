#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "flight/client/client.h"
#include "flight/command/flight.h"
#include "toolbelt/sockets.h"


#include <cstdlib>
#include <iostream>
#include <string>

namespace stagezero::flight {

FlightCommand::FlightCommand(toolbelt::InetAddress flight_addr)
    : flight_addr_(flight_addr) {
  if (absl::Status status = client_.Init(flight_addr_, "FlightCommand");
      !status.ok()) {
    std::cerr << "Can't connect to FlightDirector at address "
              << flight_addr_.ToString() << ": " << status.ToString()
              << std::endl;
    exit(1);
  }
  InitCommands();
}

void FlightCommand::InitCommands() {
  AddCommand(std::make_unique<StartCommand>(client_));
  AddCommand(std::make_unique<StopCommand>(client_));
  AddCommand(std::make_unique<StatusCommand>(client_));
}

void FlightCommand::AddCommand(std::unique_ptr<Command>cmd) {
  std::string root = cmd->Root();
  commands_.insert(std::make_pair(std::move(root), std::move(cmd)));
}

void FlightCommand::Run(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: flight <command...>\n";
    exit(1);
  }
  std::string root = argv[1];
  auto it = commands_.find(root);
  if (it == commands_.end()) {
    std::cerr << "unknown flight command " << root << std::endl;
    exit(1);
  }
  if (absl::Status status = it->second->Execute(argc, argv); !status.ok()) {
    std::cerr << status.ToString() << std::endl;
    exit(1);
  }
}

absl::Status StartCommand::Execute(int argc, char **argv) const {
  if (argc < 3) {
    return absl::InternalError("usage: flight start <subsystem>");
  }
  std::string subsystem = argv[2];
  return client_.StartSubsystem(subsystem);
}

absl::Status StopCommand::Execute(int argc, char **argv) const {
  if (argc < 3) {
    return absl::InternalError("usage: flight stop <subsystem>");
  }
  std::string subsystem = argv[2];
  return client_.StopSubsystem(subsystem);
}

absl::Status StatusCommand::Execute(int argc, char **argv) const {
  absl::StatusOr<std::vector<SubsystemStatus>> subsystems = client_.GetSubsystems();
  if (!subsystems.ok()) {
    return subsystems.status();
  }
  for (auto& subsystem : *subsystems) {
    std::cout << "Subsystem " << subsystem.subsystem << " " << AdminStateName(subsystem.admin_state) << 
    "/" << OperStateName(subsystem.oper_state) << std::endl;
  }
  return absl::OkStatus();
}

} // namespace stagezero::flight

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  toolbelt::InetAddress flight_addr("localhost", 6524);
  stagezero::flight::FlightCommand flight(flight_addr);

  flight.Run(argc, argv);
}
