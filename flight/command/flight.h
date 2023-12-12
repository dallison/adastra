// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <string>
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "toolbelt/sockets.h"
#include "flight/client/client.h"

namespace stagezero::flight {

class Command {
 public:
  Command(const std::string &root)
      : root_(root) {}
  virtual ~Command() = default;

  virtual absl::Status Execute(flight::client::Client *client, int argc, char **argv) const = 0;

  const std::string &Root() const { return root_; }

 protected:
  std::string root_;
};

class HelpCommand : public Command {
 public:
  HelpCommand() : Command( "help") {}
  absl::Status Execute(flight::client::Client *client, int argc, char **argv) const override;
};

class StartCommand : public Command {
 public:
  StartCommand() : Command("start") {}
  absl::Status Execute(flight::client::Client *client, int argc, char **argv) const override;
};

class StopCommand : public Command {
 public:
  StopCommand() : Command("stop") {}
  absl::Status Execute(flight::client::Client *client,int argc, char **argv) const override;
};

class StatusCommand : public Command {
 public:
  StatusCommand() : Command("status") {}
  absl::Status Execute(flight::client::Client *client,int argc, char **argv) const override;
};

class AlarmsCommand : public Command {
 public:
  AlarmsCommand() : Command("status") {}
  absl::Status Execute(flight::client::Client *client,int argc, char **argv) const override;
};

class AbortCommand : public Command {
 public:
  AbortCommand() : Command("abort") {}
  absl::Status Execute(flight::client::Client *client,int argc, char **argv) const override;
};

class RunCommand : public Command {
 public:
  RunCommand() : Command("run") {}
  absl::Status Execute(flight::client::Client *client,int argc, char **argv) const override;
};

class FlightCommand {
 public:
  FlightCommand(toolbelt::InetAddress flight_addr);

  void Connect(flight::client::ClientMode mode, int event_mask);
  void Run(int argc, char **argv);

 private:
  void InitCommands();
  void AddCommand(std::unique_ptr<Command> cmd);

  toolbelt::InetAddress flight_addr_;
  absl::flat_hash_map<std::string, std::unique_ptr<Command>> commands_;
  std::unique_ptr<flight::client::Client> client_;
};

}  // namespace stagezero::flight
