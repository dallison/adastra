#include "stagezero/telemetry/telemetry.h"
#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "testdata/proto/telemetry.pb.h"
#include "toolbelt/clock.h"
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

class MySystemTelemetry : public ::stagezero::SystemTelemetry {
public:
  MySystemTelemetry(::stagezero::Telemetry &telemetry)
      : SystemTelemetry(telemetry, "ZygoteCore::Telemetry") {}

  absl::Status
  HandleCommand(std::unique_ptr<::stagezero::TelemetryCommand> command,
                co::Coroutine *c) override {
    switch (command->Code()) {
    case stagezero::SystemTelemetry::kShutdownCommand: {
      auto shutdown = dynamic_cast<stagezero::ShutdownCommand *>(command.get());
      std::cout << "Shutdown command: exit_code=" << shutdown->exit_code
                << " timeout_secs=" << shutdown->timeout_secs << std::endl;
      exit(shutdown->exit_code);
      break;
    }
    default:
      std::cerr << "Unknown command code " << command->Code() << std::endl;
      break;
    }
    return absl::OkStatus();
  }
};

class MyTelemetry : public ::stagezero::TelemetryModule {
public:
  static constexpr int kPid = 100;

  MyTelemetry(::stagezero::Telemetry &telemetry)
      : ::stagezero::TelemetryModule(telemetry, "foobar") {}

  absl::StatusOr<std::unique_ptr<::stagezero::TelemetryCommand>>
  ParseCommand(const adastra::proto::telemetry::Command &command) override;

  absl::Status
  HandleCommand(std::unique_ptr<::stagezero::TelemetryCommand> command,
                co::Coroutine *c) override;
};

struct PidCommand : public ::stagezero::TelemetryCommand {
  PidCommand() = default;
  int Code() const override { return MyTelemetry::kPid; }

  void ToProto(adastra::proto::telemetry::Command &proto) const override {
    proto.mutable_command()->PackFrom(
        ::testdata::telemetry::PidTelemetryCommand());
  }

  bool FromProto(const adastra::proto::telemetry::Command &proto) override {
    if (!proto.command().Is<::testdata::telemetry::PidTelemetryCommand>()) {
      return false;
    }
    return true;
  }
};

struct PidStatus : public ::stagezero::TelemetryStatus {
  PidStatus(int pid) : pid(pid) {}

  void ToProto(adastra::proto::telemetry::Status &proto) const override {
    ::testdata::telemetry::PidTelemetryStatus pid_status;
    pid_status.set_pid(pid);
    proto.mutable_status()->PackFrom(pid_status);
  }

  bool FromProto(const adastra::proto::telemetry::Status &proto) override {
    ::testdata::telemetry::PidTelemetryStatus status;
    proto.status().UnpackTo(&status);
    pid = status.pid();
    return true;
  }
  int pid;
};

struct TimeStatus : public ::stagezero::TelemetryStatus {
  TimeStatus(uint64_t time) : time(time) {}

  void ToProto(adastra::proto::telemetry::Status &proto) const override {
    ::testdata::telemetry::TimeTelemetryStatus time_status;
    time_status.set_time(time);
    proto.mutable_status()->PackFrom(time_status);
  }

  bool FromProto(const adastra::proto::telemetry::Status &proto) override {
    ::testdata::telemetry::TimeTelemetryStatus status;
    proto.status().UnpackTo(&status);
    time = status.time();
    return true;
  }

  uint64_t time;
};

absl::StatusOr<std::unique_ptr<::stagezero::TelemetryCommand>>
MyTelemetry::ParseCommand(const adastra::proto::telemetry::Command &command) {
  if (command.command().Is<::testdata::telemetry::PidTelemetryCommand>()) {
    return std::make_unique<PidCommand>();
  }
  return nullptr;
}

absl::Status MyTelemetry::HandleCommand(
    std::unique_ptr<::stagezero::TelemetryCommand> command, co::Coroutine *c) {
  switch (command->Code()) {
  case kPid: {
    std::cout << "Pid command" << std::endl;

    PidStatus status(getpid());
    if (absl::Status s = telemetry_.SendStatus(status, c); !s.ok()) {
      return s;
    }
    break;
  }
  default:
    std::cerr << "Unknown command code " << command->Code() << std::endl;
    break;
  }
  return absl::OkStatus();
}

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
  auto t = std::make_shared<MySystemTelemetry>(telemetry);
  telemetry.AddModule(t);

  auto myt = std::make_shared<MyTelemetry>(telemetry);
  telemetry.AddModule(myt);

  telemetry.CallEvery(
      std::chrono::milliseconds(20), [&telemetry](co::Coroutine *c) {
        TimeStatus status(toolbelt::Now());
        if (absl::Status s = telemetry.SendStatus(status, c); !s.ok()) {
          std::cerr << "Failed to send command: " << s.message() << std::endl;
        }
      });
  telemetry.Run();
}
