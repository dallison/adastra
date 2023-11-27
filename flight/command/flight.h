#pragma once

#include <string>
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "toolbelt/sockets.h"
#include "flight/client/client.h"

namespace stagezero::flight {

class Command {
 public:
  Command(flight::client::Client &client, const std::string &root)
      : client_(client), root_(root) {}
  virtual ~Command() = default;

  virtual absl::Status Execute(int argc, char **argv) const = 0;

  const std::string &Root() const { return root_; }

 protected:
  flight::client::Client &client_;
  std::string root_;
};

class StartCommand : public Command {
 public:
  StartCommand(flight::client::Client &client) : Command(client, "start") {}
  absl::Status Execute(int argc, char **argv) const override;
};

class StopCommand : public Command {
 public:
  StopCommand(flight::client::Client &client) : Command(client, "stop") {}
  absl::Status Execute(int argc, char **argv) const override;
};

class StatusCommand : public Command {
 public:
  StatusCommand(flight::client::Client &client) : Command(client, "status") {}
  absl::Status Execute(int argc, char **argv) const override;
};

class AbortCommand : public Command {
 public:
  AbortCommand(flight::client::Client &client) : Command(client, "abort") {}
  absl::Status Execute(int argc, char **argv) const override;
};

class FlightCommand {
 public:
  FlightCommand(toolbelt::InetAddress flight_addr);

  void Run(int argc, char **argv);

 private:
  void InitCommands();
  void AddCommand(std::unique_ptr<Command> cmd);

  toolbelt::InetAddress flight_addr_;
  absl::flat_hash_map<std::string, std::unique_ptr<Command>> commands_;
  flight::client::Client client_;
};

}  // namespace stagezero::flight
