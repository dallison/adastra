// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "flight/command/flight.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "common/stream.h"
#include "flight/client/client.h"
#include "toolbelt/sockets.h"
#include "toolbelt/table.h"
#include "toolbelt/color.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>

ABSL_FLAG(std::string, hostname, "localhost",
          "Flight Director hostname (or IP address)");
ABSL_FLAG(int, port, 6524, "Flight Director listening port");

namespace stagezero::flight {

FlightCommand::FlightCommand(toolbelt::InetAddress flight_addr)
    : flight_addr_(flight_addr) {
  InitCommands();
}

void FlightCommand::Connect(flight::client::ClientMode mode, int event_mask) {
  client_ = std::make_unique<flight::client::Client>(mode);
  if (absl::Status status = client_->Init(flight_addr_, "FlightCommand",  event_mask);
      !status.ok()) {
    std::cerr << "Can't connect to FlightDirector at address "
              << flight_addr_.ToString() << ": " << status.ToString()
              << std::endl;
    exit(1);
  }
}

void FlightCommand::InitCommands() {
  AddCommand(std::make_unique<StartCommand>());
  AddCommand(std::make_unique<StopCommand>());
  AddCommand(std::make_unique<StatusCommand>());
  AddCommand(std::make_unique<AbortCommand>());
  AddCommand(std::make_unique<HelpCommand>());
  AddCommand(std::make_unique<AlarmsCommand>());
  AddCommand(std::make_unique<RunCommand>());
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
    bool is_run = it->first == "run";
    Connect(is_run ? flight::client::ClientMode::kNonBlocking
                               : flight::client::ClientMode::kBlocking,
                               is_run ? kOutputEvents : kNoEvents);
  }
  if (absl::Status status = it->second->Execute(client_.get(), argc, argv);
      !status.ok()) {
    std::cerr << status.ToString() << std::endl;
    exit(1);
  }
}

absl::Status HelpCommand::Execute(flight::client::Client *client, int argc,
                                  char **argv) const {
  std::cout << "Control Flight Director\n";
  std::cout << "  flight start <subsystem> - start a subsystem running\n";
  std::cout << "  flight run <subsystem> - run a subsystem interactively\n";
  std::cout << "  flight stop <subsystem> - stop a subsystem\n";
  std::cout << "  flight status - show status of all subsystems\n";
  std::cout << "  flight abort <subsystem> - abort all subsystems\n";
  std::cout << "  flight alarms - show all alarms\n";
  return absl::OkStatus();
}

absl::Status StartCommand::Execute(flight::client::Client *client, int argc,
                                   char **argv) const {
  if (argc < 3) {
    return absl::InternalError("usage: flight start <subsystem>");
  }
  std::string subsystem = argv[2];
  return client->StartSubsystem(subsystem);
}

absl::Status StopCommand::Execute(flight::client::Client *client, int argc,
                                  char **argv) const {
  if (argc < 3) {
    return absl::InternalError("usage: flight stop <subsystem>");
  }
  std::string subsystem = argv[2];
  return client->StopSubsystem(subsystem);
}

absl::Status StatusCommand::Execute(flight::client::Client *client, int argc,
                                    char **argv) const {
  absl::StatusOr<std::vector<SubsystemStatus>> subsystems =
      client->GetSubsystems();
  if (!subsystems.ok()) {
    return subsystems.status();
  }
  struct winsize win;
  int cols = 80;
  if (ioctl(0, TIOCGWINSZ, &win) == 0) {
    cols = win.ws_col;
  };

  toolbelt::Table table({"subsystem", "admin", "oper", "processes"});
  for (auto &subsystem : *subsystems) {
    table.AddRow();
    toolbelt::color::Color admin_color;
    toolbelt::color::Color oper_color;

    switch (subsystem.admin_state) {
    case AdminState::kOffline:
      admin_color = toolbelt::color::BoldMagenta();
      break;
    case AdminState::kOnline:
      admin_color = toolbelt::color::BoldGreen();
      break;
    }

    switch (subsystem.oper_state) {
    case OperState::kOffline:
      oper_color = toolbelt::color::BoldMagenta();
      break;
    case OperState::kBroken:
      oper_color = toolbelt::color::BoldRed();
      break;
    case OperState::kOnline:
      oper_color = toolbelt::color::BoldGreen();
      break;
    case OperState::kRestarting:
    case OperState::kStartingChildren:
    case OperState::kStartingProcesses:
    case OperState::kStoppingChildren:
    case OperState::kStoppingProcesses:
      oper_color = toolbelt::color::BoldYellow();
      break;
    }
    table.SetCell(0, toolbelt::Table::MakeCell(subsystem.subsystem));
    table.SetCell(1, toolbelt::Table::MakeCell(
                         AdminStateName(subsystem.admin_state), admin_color));
    table.SetCell(2, toolbelt::Table::MakeCell(
                         OperStateName(subsystem.oper_state), oper_color));
    table.SetCell(3, toolbelt::Table::MakeCell(
                         absl::StrFormat("%d", subsystem.processes.size())));
  }
  table.Print(cols, std::cout);
  return absl::OkStatus();
}

absl::Status AlarmsCommand::Execute(flight::client::Client *client, int argc,
                                    char **argv) const {
  absl::StatusOr<std::vector<Alarm>> alarms = client->GetAlarms();
  if (!alarms.ok()) {
    return alarms.status();
  }
  for (auto &alarm : *alarms) {
    std::cout << alarm << std::endl;
  }
  return absl::OkStatus();
}

absl::Status AbortCommand::Execute(flight::client::Client *client, int argc,
                                   char **argv) const {
  if (argc < 3) {
    return absl::InternalError("usage: flight abort <reason>");
  }
  std::string reason = argv[2];

  return client->Abort(reason);
}

absl::Status RunCommand::Execute(flight::client::Client *client, int argc,
                                 char **argv) const {
  if (argc < 3) {
    return absl::InternalError("usage: flight run <subsystem>");
  }
  std::string subsystem = argv[2];

  struct winsize win;
  ioctl(0, TIOCGWINSZ, &win); // Might fail.
  Terminal term = {
      .name = getenv("TERM"), .rows = win.ws_row, .cols = win.ws_col};

  if (absl::Status status = client->StartSubsystem(
          subsystem, client::RunMode::kInteractive, &term);
      !status.ok()) {
    return status;
  }

  if (absl::Status status = client->WaitForSubsystemState(
          subsystem, AdminState::kOnline, OperState::kOnline);
      !status.ok()) {
    return status;
  }
  struct termios cooked; /// Cooked mode terminal state.

  tcgetattr(STDIN_FILENO, &cooked);
  struct termios raw = cooked; /// Raw mode terminal state.

#if defined(__APPLE__)
  raw.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | /*INLCR | */ IGNCR |
                   /* ICRNL |*/ IXON);
  raw.c_oflag &= ~OPOST;
  raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  raw.c_cflag &= ~(CSIZE | PARENB);
  raw.c_cflag |= CS8;
#else
  raw.c_oflag &= ~(OLCUC | OCRNL | ONLRET | XTABS);
  raw.c_iflag &= ~(ICRNL | IGNCR | INLCR);
  raw.c_lflag &= ~(ICANON | XCASE | ECHO | ECHOE | ECHOK | ECHONL | ECHOCTL |
                   ECHOPRT | ECHOKE);
#endif

  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  tcsetattr(0, TCSANOW, &raw);

  co::CoroutineScheduler scheduler;

  co::Coroutine read_from_flight(
      scheduler,
      [client](co::Coroutine *c) {
        for (;;) {
          absl::StatusOr<std::shared_ptr<Event>> e = client->WaitForEvent(c);
          if (!e.ok()) {
            std::cout << e.status() << std::endl;
            break;
          }
          auto event = *e;
          if (event->type == EventType::kOutput) {
            auto data = std::get<2>(event->event).data;
            ::write(STDOUT_FILENO, data.data(), data.size());
          }
        }
      },
      "read_from_flight");

  co::Coroutine write_to_flight(
      scheduler,
      [&subsystem, client](co::Coroutine *c) {
        for (;;) {
          c->Wait(STDIN_FILENO, POLLIN);
          char buf[1];
          ssize_t n = ::read(STDIN_FILENO, buf, 1);
          if (absl::Status status = client->SendInput(subsystem, STDIN_FILENO,
                                                      std::string(buf, n), c);
              !status.ok()) {
            std::cout << status << std::endl;
            break;
          }
        }
      },
      "write_to_flight");

  scheduler.Run();

  tcsetattr(0, TCSANOW, &cooked);

  return absl::OkStatus();
}

} // namespace stagezero::flight

int main(int argc, char **argv) {
  absl::ParseCommandLine(argc, argv);

  toolbelt::InetAddress flight_addr(absl::GetFlag(FLAGS_hostname),
                                    absl::GetFlag(FLAGS_port));
  stagezero::flight::FlightCommand flight(flight_addr);

  flight.Run(argc, argv);
}
