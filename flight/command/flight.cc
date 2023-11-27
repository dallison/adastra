// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "flight/command/flight.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "flight/client/client.h"
#include "toolbelt/sockets.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace stagezero::flight {

FlightCommand::FlightCommand(toolbelt::InetAddress flight_addr)
    : flight_addr_(flight_addr) {
  InitCommands();
}

void FlightCommand::Connect() {
  if (absl::Status status = client_.Init(flight_addr_, "FlightCommand");
      !status.ok()) {
    std::cerr << "Can't connect to FlightDirector at address "
              << flight_addr_.ToString() << ": " << status.ToString()
              << std::endl;
    exit(1);
  }
}

void FlightCommand::InitCommands() {
  AddCommand(std::make_unique<StartCommand>(client_));
  AddCommand(std::make_unique<StopCommand>(client_));
  AddCommand(std::make_unique<StatusCommand>(client_));
  AddCommand(std::make_unique<AbortCommand>(client_));
  AddCommand(std::make_unique<HelpCommand>(client_));
  AddCommand(std::make_unique<AlarmsCommand>(client_));
}

void FlightCommand::AddCommand(std::unique_ptr<Command> cmd) {
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
  if (it->first != "help") {
    Connect();
  }
  if (absl::Status status = it->second->Execute(argc, argv); !status.ok()) {
    std::cerr << status.ToString() << std::endl;
    exit(1);
  }
}

absl::Status HelpCommand::Execute(int argc, char **argv) const {
  std::cout << "Control Flight Director\n";
  std::cout << "  flight start <subsystem> - start a subsystem running\n";
  std::cout << "  flight stop <subsystem> - stop a subsystem\n";
  std::cout << "  flight status - show status of all subsystems\n";
  std::cout << "  flight abort <subsystem> - abort all subsystems\n";
  std::cout << "  flight alarms - show all alarms\n";
  return absl::OkStatus();
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
  absl::StatusOr<std::vector<SubsystemStatus>> subsystems =
      client_.GetSubsystems();
  if (!subsystems.ok()) {
    return subsystems.status();
  }
  for (auto &subsystem : *subsystems) {
    std::cout << "Subsystem " << subsystem.subsystem << " "
              << AdminStateName(subsystem.admin_state) << "/"
              << OperStateName(subsystem.oper_state) << std::endl;
  }
  return absl::OkStatus();
}

absl::Status AlarmsCommand::Execute(int argc, char **argv) const {
  absl::StatusOr<std::vector<Alarm>> alarms =
      client_.GetAlarms();
  if (!alarms.ok()) {
    return alarms.status();
  }
  for (auto &alarm : *alarms) {
    std::cout << alarm << std::endl;
  }
  return absl::OkStatus();
}
absl::Status AbortCommand::Execute(int argc, char **argv) const {
  if (argc < 3) {
    return absl::InternalError("usage: flight abort <reason>");
  }
  std::string reason = argv[2];

  return client_.Abort(reason);
}
} // namespace stagezero::flight

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  toolbelt::InetAddress flight_addr("localhost", 6524);
  stagezero::flight::FlightCommand flight(flight_addr);

  flight.Run(argc, argv);
}
