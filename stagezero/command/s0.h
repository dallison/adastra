#pragma once

#include <string>
#include "absl/status/status.h"
#include "toolbelt/sockets.h"
#include "absl/container/flat_hash_map.h"
#include "stagezero/client/client.h"

namespace adastra::stagezero {

class Command {
public:
  Command(const std::string &root) : root_(root) {}
  virtual ~Command() = default;

  virtual absl::Status Execute(Client *client, int argc,
                               char **argv) const = 0;

  const std::string &Root() const { return root_; }

protected:
  std::string root_;
};

class HelpCommand : public Command {
public:
  HelpCommand() : Command("help") {}
  absl::Status Execute(Client *client, int argc,
                       char **argv) const override;
};

class LaunchStaticCommand : public Command {
public:
  LaunchStaticCommand() : Command("launch-static") {}
  absl::Status Execute(Client *client, int argc,
                       char **argv) const override;
};

class StopCommand : public Command {
public:
  StopCommand() : Command("stop") {}
  absl::Status Execute(Client *client, int argc,
                       char **argv) const override;
};

class StageZeroCommand {
public:
  StageZeroCommand(toolbelt::InetAddress flight_addr);

  void Connect();
  void Run(int argc, char **argv);

  static std::shared_ptr<control::Event> WaitForEvent(Client* client);

private:
  void InitCommands();
  void AddCommand(std::unique_ptr<Command> cmd);

  
  toolbelt::InetAddress stagezero_addr_;
  absl::flat_hash_map<std::string, std::unique_ptr<Command>> commands_;
  std::unique_ptr<Client> client_;
};
}
