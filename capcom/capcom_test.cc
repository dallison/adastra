// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "capcom/capcom.h"
#include "capcom/client/client.h"
#include "capcom/subsystem.h"
#include "proto/telemetry.pb.h"
#include "stagezero/stagezero.h"
#include "testdata/proto/telemetry.pb.h"
#include <gtest/gtest.h>

#include <fstream>
#include <inttypes.h>
#include <memory>
#include <signal.h>
#include <sstream>
#include <sys/resource.h>
#include <thread>

ABSL_FLAG(bool, start_capcom, true, "Start capcom");
ABSL_FLAG(bool, start_stagezero, true, "Start stagezero");

using AdminState = adastra::AdminState;
using OperState = adastra::OperState;
using SubsystemStatus = adastra::SubsystemStatus;
using Event = adastra::Event;
using EventType = adastra::EventType;
using ClientMode = adastra::capcom::client::ClientMode;

void SignalHandler(int sig);
void StageZeroSignalHandler(int sig);
void SignalQuitHandler(int sig);

class CapcomTest : public ::testing::Test {
public:
  // We run one server for the duration of the whole test suite.
  static void SetUpTestSuite() {
    if (!absl::GetFlag(FLAGS_start_capcom)) {
      return;
    }
    StartStageZero();
    StartCapcom();
  }

  static void StartStageZero(int port = 6522) {
    if (!absl::GetFlag(FLAGS_start_stagezero)) {
      return;
    }
    fprintf(stderr, "Starting StageZero process\n");

    // Capcom will write to this pipe to notify us when it
    // has started and stopped.  This end of the pipe is blocking.
    (void)pipe(stagezero_pipe_);

    stagezero_addr_ = toolbelt::InetAddress("localhost", port);
    stagezero_ = std::make_unique<adastra::stagezero::StageZero>(
        stagezero_scheduler_, stagezero_addr_, true, "/tmp", "debug", "",
        stagezero_pipe_[1]);

    stagezero_pid_ = fork();
    if (stagezero_pid_ == 0) {
      capcom_thread_.release();
      signal(SIGTERM, StageZeroSignalHandler);
      signal(SIGINT, StageZeroSignalHandler);
      // Child process.
      absl::Status s = stagezero_->Run();
      if (!s.ok()) {
        fprintf(stderr, "Error running stagezero: %s\n", s.ToString().c_str());
      }
      exit(1);
    }

    // Wait stagezero to tell us that it's running.
    char buf[8];
    (void)::read(stagezero_pipe_[0], buf, 8);
    signal(SIGINT, SignalHandler);
    signal(SIGQUIT, SignalQuitHandler);
  }

  static void StartCapcom(int port = 6523, int stagezero_port = 6522) {
    printf("Starting Capcom\n");

    // Capcom will write to this pipe to notify us when it
    // has started and stopped.  This end of the pipe is blocking.
    (void)pipe(capcom_pipe_);

    capcom_addr_ = toolbelt::InetAddress("localhost", port);
    capcom_ = std::make_unique<adastra::capcom::Capcom>(
        capcom_scheduler_, capcom_addr_, true, stagezero_port, "", "debug",
        false, capcom_pipe_[1]);

    // Start capcom running in a thread.
    capcom_thread_ = std::make_unique<std::thread>([]() {
      absl::Status s = capcom_->Run();
      if (!s.ok()) {
        fprintf(stderr, "Error running capcom: %s\n", s.ToString().c_str());
        exit(1);
      }
    });

    // Wait stagezero to tell us that it's running.
    char buf[8];
    (void)::read(capcom_pipe_[0], buf, 8);
    std::cout << "capcom running\n";
  }

  static void TearDownTestSuite() {
    if (!absl::GetFlag(FLAGS_start_capcom)) {
      return;
    }
    StopCapcom();
    StopStageZero();
  }

  static void StopCapcom() {
    printf("Stopping Capcom\n");
    capcom_->Stop();

    // Wait for server to tell us that it's stopped.
    WaitForStop(capcom_pipe_[0], *capcom_thread_);
  }

  static void StopStageZero() {
    if (stagezero_pid_ > 0) {
      fprintf(stderr, "Stopping StageZero process\n");
      kill(stagezero_pid_, SIGTERM);
      int status;
      waitpid(stagezero_pid_, &status, 0);
      stagezero_pid_ = 0;
    }
  }

  static void WaitForStop(int fd, std::thread &t) {
    char buf[8];
    (void)::read(fd, buf, 8);
    t.join();
  }

  static void WaitForStop() {
    if (stagezero_pid_ > 0) {
      int status;
      waitpid(stagezero_pid_, &status, 0);
      stagezero_pid_ = 0;
    }
    WaitForStop(capcom_pipe_[0], *capcom_thread_);
  }

  void SetUp() override { signal(SIGPIPE, SIG_IGN); }
  void TearDown() override {}

  void InitClient(adastra::capcom::client::Client &client,
                  const std::string &name,
                  int event_mask = adastra::kSubsystemStatusEvents |
                                   adastra::kOutputEvents |
                                   adastra::kParameterEvents |
                                   adastra::kTelemetryEvents) {
    absl::Status s = client.Init(CapcomAddr(), name, event_mask);
    std::cout << "Init status: " << s << std::endl;
    ASSERT_TRUE(s.ok());
  }

  Event WaitForState(adastra::capcom::client::Client &client,
                     std::string subsystem, AdminState admin_state,
                     OperState oper_state,
                     std::stringstream *output = nullptr) {
    std::cout << "waiting for subsystem state change " << subsystem << " "
              << admin_state << " " << oper_state << std::endl;
    for (int retry = 0; retry < 30; retry++) {
      absl::StatusOr<std::shared_ptr<Event>> e = client.WaitForEvent();
      if (!e.ok()) {
        std::cerr << e.status().ToString() << std::endl;
        return {};
      }
      std::shared_ptr<Event> event = *e;
      std::cerr << "WaitForState: event " << (int)event->type << "\n";
      if (event->type == EventType::kSubsystemStatus) {
        SubsystemStatus &s = std::get<0>(event->event);
        std::cerr << s.subsystem << " " << s.admin_state << " " << s.oper_state
                  << std::endl;
        std::cerr << "waiting for subsystem state change " << subsystem << " "
                  << admin_state << " " << oper_state << std::endl;
        if (s.subsystem == subsystem && s.admin_state == admin_state &&
            s.oper_state == oper_state) {
          std::cerr << "event OK" << std::endl;
          return *event;
        }
      } else if (event->type == EventType::kOutput) {
        if (output != nullptr) {
          (*output) << std::get<2>(event->event).data;
        }
      }
    }
    EXPECT_TRUE(false);
    return {};
  }

  std::string WaitForOutput(adastra::capcom::client::Client &client,
                            std::string match,
                            std::string *prev_output = nullptr) {
    std::cout << "waiting for output " << match << "\n";
    std::stringstream s;
    if (prev_output != nullptr) {
      s << *prev_output;
    }
    if (s.str().find(match) != std::string::npos) {
      return s.str();
    }
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<adastra::Event>> e = client.WaitForEvent();
      std::cout << e.status().ToString() << "\n";
      if (!e.ok()) {
        return s.str();
      }
      std::shared_ptr<adastra::Event> event = *e;
      std::cerr << "event: " << (int)event->type << std::endl;
      if (event->type == adastra::EventType::kOutput) {
        s << std::get<2>(event->event).data;
        std::cout << s.str() << std::endl;
        if (s.str().find(match) != std::string::npos) {
          return s.str();
        }
      }
    }
    abort();
  }

  void WaitForAlarm(adastra::capcom::client::Client &client,
                    adastra::Alarm::Type type,
                    adastra::Alarm::Severity severity,
                    adastra::Alarm::Reason reason) {
    std::cout << "waiting for alarm\n";
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<adastra::Event>> e = client.WaitForEvent();
      ASSERT_TRUE(e.ok());

      std::shared_ptr<adastra::Event> event = *e;
      std::cerr << "event: " << (int)event->type << std::endl;
      if (event->type == adastra::EventType::kAlarm) {
        adastra::Alarm alarm = std::get<1>(event->event);
        if (alarm.type == type && alarm.severity == severity &&
            alarm.reason == reason) {
          std::cout << "alarm received\n";
          return;
        }
      }
    }
    FAIL();
  }

  void WaitForParameterUpdate(adastra::capcom::client::Client &client,
                              const std::string &name,
                              adastra::parameters::Value v) {
    std::cout << "waiting for parameter update\n";
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<adastra::Event>> e = client.WaitForEvent();
      std::cerr << e.status() << std::endl;
      ASSERT_TRUE(e.ok());

      std::shared_ptr<adastra::Event> event = *e;
      std::cerr << "event: " << (int)event->type << std::endl;
      if (event->type == adastra::EventType::kParameterUpdate) {
        const adastra::parameters::Parameter &p = std::get<4>(event->event);
        std::cerr << "update parameter: " << p.name << " " << p.value
                  << std::endl;
        if (p.name == name && p.value == v) {
          std::cout << "parameter update received\n";
          return;
        }
      }
    }
    FAIL();
  }

  void WaitForParameterDelete(adastra::capcom::client::Client &client,
                              const std::string &name) {
    std::cout << "waiting for parameter delete\n";
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<adastra::Event>> e = client.WaitForEvent();
      std::cerr << e.status() << std::endl;
      ASSERT_TRUE(e.ok());

      std::shared_ptr<adastra::Event> event = *e;
      std::cerr << "event: " << (int)event->type << std::endl;
      if (event->type == adastra::EventType::kParameterDelete) {
        std::string pname = std::get<5>(event->event);
        std::cerr << "delete parameter: " << pname << std::endl;
        if (pname == name) {
          std::cout << "parameter delete received\n";
          return;
        }
      }
    }
    FAIL();
  }

  std::unique_ptr<::adastra::proto::TelemetryEvent>
  WaitForTelemetryEvent(adastra::capcom::client::Client &client) {
    std::cout << "waiting for telemetry event " << std::endl;
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<adastra::Event>> e = client.WaitForEvent();

      std::cout << e.status().ToString() << "\n";
      EXPECT_TRUE(e.ok());
      std::shared_ptr<adastra::Event> event = *e;

      if (event->type == adastra::EventType::kTelemetry) {
        return std::make_unique<::adastra::proto::TelemetryEvent>(
            std::get<::adastra::proto::TelemetryEvent>(event->event));
      }
      return nullptr;
    }
    EXPECT_TRUE(false);
    return nullptr;
  }

  void SendInput(adastra::capcom::client::Client &client, std::string subsystem,
                 std::string process, int fd, std::string s) {
    absl::Status status = client.SendInput(subsystem, process, fd, s);
    ASSERT_TRUE(status.ok());
  }

  static const toolbelt::InetAddress &CapcomAddr() { return capcom_addr_; }

  static co::CoroutineScheduler &CapcomScheduler() { return capcom_scheduler_; }
  static co::CoroutineScheduler &StageZeroScheduler() {
    return stagezero_scheduler_;
  }

  static adastra::capcom::Capcom *Capcom() { return capcom_.get(); }
  static adastra::stagezero::StageZero *StageZero() { return stagezero_.get(); }

private:
  static co::CoroutineScheduler capcom_scheduler_;
  static int capcom_pipe_[2];
  static std::unique_ptr<adastra::capcom::Capcom> capcom_;
  static std::unique_ptr<std::thread> capcom_thread_;
  static toolbelt::InetAddress capcom_addr_;

  static co::CoroutineScheduler stagezero_scheduler_;
  static int stagezero_pipe_[2];
  static std::unique_ptr<adastra::stagezero::StageZero> stagezero_;
  static toolbelt::InetAddress stagezero_addr_;
  static int stagezero_pid_;
};

co::CoroutineScheduler CapcomTest::capcom_scheduler_;
int CapcomTest::capcom_pipe_[2];
std::unique_ptr<adastra::capcom::Capcom> CapcomTest::capcom_;
std::unique_ptr<std::thread> CapcomTest::capcom_thread_;
toolbelt::InetAddress CapcomTest::capcom_addr_;

co::CoroutineScheduler CapcomTest::stagezero_scheduler_;
int CapcomTest::stagezero_pipe_[2];
std::unique_ptr<adastra::stagezero::StageZero> CapcomTest::stagezero_;
toolbelt::InetAddress CapcomTest::stagezero_addr_;
int CapcomTest::stagezero_pid_;

void SignalHandler(int sig) {
  printf("Signal %d\n", sig);
  CapcomTest::Capcom()->Stop();
  CapcomTest::StageZero()->Stop();
  CapcomTest::WaitForStop();

  signal(sig, SIG_DFL);
  raise(sig);
}

void StageZeroSignalHandler(int sig) {
  printf("StageZero Signal %d\n", sig);
  CapcomTest::StageZero()->Stop();
}

void SignalQuitHandler(int sig) {
  printf("Signal %d\n", sig);
  CapcomTest::CapcomScheduler().Show();
  CapcomTest::Capcom()->Stop();
  CapcomTest::StageZero()->Stop();
  CapcomTest::WaitForStop();

  signal(sig, SIG_DFL);
  raise(sig);
}

TEST_F(CapcomTest, Init) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");
}

TEST_F(CapcomTest, Compute) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  // Should work.
  absl::Status status =
      client.AddCompute("compute1", toolbelt::InetAddress("localhost", 6522));
  ASSERT_TRUE(status.ok());

  // Duplicate.
  status =
      client.AddCompute("compute1", toolbelt::InetAddress("localhost", 6522));
  ASSERT_FALSE(status.ok());

  // Bad host name
  status =
      client.AddCompute("compute1", toolbelt::InetAddress("foobarfoo", 6522));
  ASSERT_FALSE(status.ok());

  // Bad port.
  status =
      client.AddCompute("compute1", toolbelt::InetAddress("localhost", 6525));
  ASSERT_FALSE(status.ok());

  // No such compute.
  status = client.RemoveCompute("compute2");
  ASSERT_FALSE(status.ok());

  // Will work.
  status = client.RemoveCompute("compute1");
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, SimpleSubsystem) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  std::cerr << status << std::endl;
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("foobar", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, SimpleSubsystemCompute) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status =
      client.AddCompute("localhost", toolbelt::InetAddress("localhost", 6522));
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "foobar", {.static_processes = {{
                     .name = "loop",
                     .executable = "${runfiles_dir}/_main/testdata/loop",
                     .compute = "localhost",
                 }}});
  ASSERT_TRUE(status.ok());

  // Unknown compute should fail.
  status = client.AddSubsystem(
      "foobar2", {.static_processes = {{
                      .name = "loop",
                      .executable = "${runfiles_dir}/_main/testdata/loop",
                      .compute = "notknown",
                  }}});
  ASSERT_FALSE(status.ok());

  status = client.RemoveSubsystem("foobar", false);
  ASSERT_TRUE(status.ok());

  status = client.RemoveCompute("localhost");
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, SubsystemWithMultipleCompute) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status =
      client.AddCompute("localhost1", toolbelt::InetAddress("localhost", 6522));
  ASSERT_TRUE(status.ok());

  status =
      client.AddCompute("localhost2", toolbelt::InetAddress("localhost", 6522));
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "foobar", {.static_processes = {
                     {
                         .name = "loop1",
                         .executable = "${runfiles_dir}/_main/testdata/loop",
                         .compute = "localhost1",
                     },
                     {
                         .name = "loop2",
                         .executable = "${runfiles_dir}/_main/testdata/loop",
                         .compute = "localhost2",
                     }}});
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("foobar", false);
  ASSERT_TRUE(status.ok());

  status = client.RemoveCompute("localhost1");
  ASSERT_TRUE(status.ok());

  status = client.RemoveCompute("localhost2");
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, StartSimpleSubsystem) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar1",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, StartSimpleSubsystemTelemetry) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar1", {.static_processes = {{
                      .name = "telemetry",
                      .executable = "${runfiles_dir}/_main/testdata/telemetry",
                      .telemetry = true,
                  }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, StartSimpleSubsystemTelemetryCommand) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar1", {.static_processes = {{
                      .name = "telemetry",
                      .executable = "${runfiles_dir}/_main/testdata/telemetry",
                      .telemetry = true,
                  }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);

  // This will cause the process to exit and it will be restarted.
  stagezero::ShutdownCommand cmd(1, 2);
  status = client.SendTelemetryCommandToSubsystem("foobar1", cmd);
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);

  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
}

struct PidCommand : public ::stagezero::TelemetryCommand {
  PidCommand() = default;

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

TEST_F(CapcomTest, StartSimpleSubsystemCustomTelemetryCommand) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar1", {.static_processes = {{
                      .name = "telemetry",
                      .executable = "${runfiles_dir}/_main/testdata/telemetry",
                      .telemetry = true,
                  }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);

  // Send a custom telemetry command to get the PID.
  PidCommand cmd;
  absl::Status s = client.SendTelemetryCommandToSubsystem("foobar1", cmd);

  auto ts = WaitForTelemetryEvent(client);
  ASSERT_TRUE(ts != nullptr);
  ASSERT_TRUE(
      ts->telemetry().status().Is<::testdata::telemetry::PidTelemetryStatus>());
  std::cerr << ts->DebugString();

  for (int i = 0; i < 10; i++) {
    ts = WaitForTelemetryEvent(client);
    if (ts->telemetry()
            .status()
            .Is<::testdata::telemetry::TimeTelemetryStatus>()) {
      ::testdata::telemetry::TimeTelemetryStatus status;
      auto ok = ts->telemetry().status().UnpackTo(&status);
      ASSERT_TRUE(ok);
      std::cout << "Time: " << status.time() << std::endl;
    }
  }

  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
}

// Start a subsystem with no stagezero, then start stagezero.
TEST_F(CapcomTest, StartSimpleSubsystemDelayedStageZero) {
  StopStageZero();
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar1",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  // Try to start a subsystem without stagezero running.  We will get into
  // connecting state but will not go online.
  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kConnecting);

  // Now start stagezero and we should go online.
  StartStageZero();
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);

  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, StartSimpleSubsystemNoStageZeroAbort) {
  StopStageZero();
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar1",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  // Try to start a subsystem without stagezero running.  We will get into
  // connecting state but will not go online.
  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kConnecting);

  // While the subsystem is connecting, stop it.  It should go offline.
  std::cerr << "Stopping subsystem\n";
  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
  StartStageZero();
}

TEST_F(CapcomTest, StartSimpleSubsystemStageZeroDisconnect) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar1",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  // Try to start a subsystem without stagezero running.  We will get into
  // connecting state but will not go online.
  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);

  sleep(1);

  StopStageZero();

  // We will go into connecting state until stagezero starts.
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kConnecting);

  StartStageZero();

  // Process should start again.
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);

  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, OneshotOK) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "oneshot", {.static_processes = {{
                      .name = "oneshot",
                      .executable = "${runfiles_dir}/_main/testdata/oneshot",
                      .oneshot = true,
                  }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("oneshot");
  ASSERT_TRUE(status.ok());

  sleep(2);

  status = client.StopSubsystem("oneshot");
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("oneshot", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, OneshotFail) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "oneshot", {.static_processes = {{
                      .name = "oneshot",
                      .executable = "${runfiles_dir}/_main/testdata/oneshot",
                      .args = {"--fail"},
                      .notify = true,
                      .oneshot = true,
                  }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("oneshot");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "oneshot", AdminState::kOnline, OperState::kOnline);
  WaitForState(client, "oneshot", AdminState::kOnline, OperState::kBroken);

  status = client.StopSubsystem("oneshot");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "oneshot", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("oneshot", false);
  ASSERT_TRUE(status.ok());

  // To see the log messages.
  sleep(1);
}

TEST_F(CapcomTest, OneshotSignal) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "oneshot", {.static_processes = {{
                      .name = "oneshot",
                      .executable = "${runfiles_dir}/_main/testdata/oneshot",
                      .args = {"--signal"},
                      .notify = true,
                      .oneshot = true,
                  }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("oneshot");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "oneshot", AdminState::kOnline, OperState::kOnline);

  WaitForState(client, "oneshot", AdminState::kOnline, OperState::kBroken);

  status = client.StopSubsystem("oneshot");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "oneshot", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("oneshot", false);
  ASSERT_TRUE(status.ok());

  // To see the log messages.
  sleep(1);
}

TEST_F(CapcomTest, OneshotKill) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "oneshot", {.static_processes = {{
                      .name = "oneshot",
                      .executable = "${runfiles_dir}/_main/testdata/oneshot",
                      .args = {"--signal"},
                      .notify = true,
                      .oneshot = true,
                  }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("oneshot");
  ASSERT_TRUE(status.ok());

  Event e =
      WaitForState(client, "oneshot", AdminState::kOnline, OperState::kOnline);

  // Kill the process.
  SubsystemStatus s = std::get<0>(e.event);
  ASSERT_EQ(1, s.processes.size());
  int pid = s.processes[0].pid;
  kill(pid, SIGINT);

  WaitForState(client, "oneshot", AdminState::kOnline, OperState::kBroken);

  status = client.StopSubsystem("oneshot");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "oneshot", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("oneshot", false);
  ASSERT_TRUE(status.ok());

  // To see the log messages.
  sleep(1);
}

TEST_F(CapcomTest, StartSimpleSubsystemCompute) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status =
      client.AddCompute("localhost", toolbelt::InetAddress("localhost", 6522));
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "foobar1", {.static_processes = {
                      {
                          .name = "loop1",
                          .executable = "${runfiles_dir}/_main/testdata/loop",
                          .compute = "localhost",
                      },
                      {
                          .name = "loop2",
                          .executable = "${runfiles_dir}/_main/testdata/loop",
                          .compute = "localhost",
                      }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());

  status = client.RemoveCompute("localhost");
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, StartSimpleSubsystemWithMultipleCompute) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status =
      client.AddCompute("localhost1", toolbelt::InetAddress("localhost", 6522));
  ASSERT_TRUE(status.ok());

  status =
      client.AddCompute("localhost2", toolbelt::InetAddress("localhost", 6522));
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "foobar1", {.static_processes = {
                      {
                          .name = "loop1",
                          .executable = "${runfiles_dir}/_main/testdata/loop",
                          .compute = "localhost1",
                      },
                      {
                          .name = "loop2",
                          .executable = "${runfiles_dir}/_main/testdata/loop",
                          .compute = "localhost2",
                      },
                  }});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());

  status = client.RemoveCompute("localhost1");
  ASSERT_TRUE(status.ok());

  status = client.RemoveCompute("localhost2");
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, RestartSimpleSubsystem) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar1",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  Event e =
      WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Kill the process.
  SubsystemStatus s = std::get<0>(e.event);
  ASSERT_EQ(1, s.processes.size());
  int pid = s.processes[0].pid;
  kill(pid, SIGTERM);

  // Wait for the subsytem to go into restarting, then back online.
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kRestarting);
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Stop the subsystem
  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, RecoverSimpleSubsystem) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar1",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }},
       .max_restarts = 0});
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  Event e =
      WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Kill the process.
  SubsystemStatus s = std::get<0>(e.event);
  ASSERT_EQ(1, s.processes.size());
  int pid = s.processes[0].pid;
  kill(pid, SIGTERM);

  // wait for the subsytem to go into broken state.
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kBroken);
  sleep(1);

  // Now recover the subsystem by restarting it.
  status = client.RestartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);

  // Stop the subsystem
  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, RestartSubsystemParentOffline) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar1",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  // This won't go online.  It has processes.
  status = client.AddSubsystem(
      "foobar2",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }},
       .children = {
           "foobar1",
       }});
  ASSERT_TRUE(status.ok());

  // This won't go online.
  status = client.AddSubsystem("foobar3", {.children = {
                                               "foobar2",
                                           }});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "foobar3", AdminState::kOffline, OperState::kOffline);

  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  Event e =
      WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Kill the process.
  SubsystemStatus s = std::get<0>(e.event);
  ASSERT_EQ(1, s.processes.size());
  int pid = s.processes[0].pid;
  kill(pid, SIGTERM);

  // Wait for the subsytem to go into restarting, then back online.
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kRestarting);
  WaitForState(client, "foobar1", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Stop the subsystem
  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "foobar1", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
  status = client.RemoveSubsystem("foobar2", false);
  ASSERT_TRUE(status.ok());
  status = client.RemoveSubsystem("foobar3", false);
  std::cerr << status << std::endl;
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, StartSimpleSubsystemTree) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "child",
      {.static_processes = {{
           .name = "loop1", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "parent",
      {.static_processes = {{
           .name = "loop2", .executable = "${runfiles_dir}/_main/testdata/loop",
       }},
       .children = {
           "child",
       }});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("parent");
  ASSERT_TRUE(status.ok());

  status = client.StopSubsystem("parent");
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("child", false);
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("parent", false);
  ASSERT_TRUE(status.ok());
  sleep(1);
}

TEST_F(CapcomTest, RestartProcess) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "manual",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }},
       .restart_policy = adastra::capcom::client::RestartPolicy::kAutomatic});
  ASSERT_TRUE(status.ok());
  WaitForState(client, "manual", AdminState::kOffline, OperState::kOffline);

  status = client.StartSubsystem("manual");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Restart the process.
  status = client.RestartProcesses("manual", {"loop"});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOnline,
               OperState::kRestartingProcesses);
  WaitForState(client, "manual", AdminState::kOnline,
               OperState::kStartingProcesses);

  sleep(1);
  WaitForState(client, "manual", AdminState::kOnline, OperState::kOnline);

  // Stop the subsystem
  status = client.StopSubsystem("manual");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("manual", false);
  ASSERT_TRUE(status.ok());
}

// Restart two processes and keep one running.
TEST_F(CapcomTest, RestartProcess2) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "manual",
      {.static_processes =
           {{
                .name = "loop1",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop2",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop3",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            }},
       .restart_policy = adastra::capcom::client::RestartPolicy::kAutomatic});
  ASSERT_TRUE(status.ok());
  WaitForState(client, "manual", AdminState::kOffline, OperState::kOffline);

  status = client.StartSubsystem("manual");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Restart processes loop1 and loop3, loop2 will not be restarted.
  status = client.RestartProcesses("manual", {"loop1", "loop3"});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOnline,
               OperState::kRestartingProcesses);
  WaitForState(client, "manual", AdminState::kOnline,
               OperState::kStartingProcesses);

  sleep(1);
  WaitForState(client, "manual", AdminState::kOnline, OperState::kOnline);

  // Stop the subsystem
  status = client.StopSubsystem("manual");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("manual", false);
  ASSERT_TRUE(status.ok());
}

// Restart all processes.
TEST_F(CapcomTest, RestartAllProcesses) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "manual",
      {.static_processes =
           {{
                .name = "loop1",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop2",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop3",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            }},
       .restart_policy = adastra::capcom::client::RestartPolicy::kAutomatic});
  ASSERT_TRUE(status.ok());
  WaitForState(client, "manual", AdminState::kOffline, OperState::kOffline);

  status = client.StartSubsystem("manual");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Restart the processes.
  status = client.RestartProcesses("manual", {});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOnline,
               OperState::kRestartingProcesses);
  WaitForState(client, "manual", AdminState::kOnline,
               OperState::kStartingProcesses);

  sleep(1);
  WaitForState(client, "manual", AdminState::kOnline, OperState::kOnline);

  // Stop the subsystem
  status = client.StopSubsystem("manual");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("manual", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, ManualRestartSimpleSubsystem) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "manual",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }},
       .restart_policy = adastra::capcom::client::RestartPolicy::kManual});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOffline, OperState::kOffline);

  status = client.StartSubsystem("manual");
  ASSERT_TRUE(status.ok());
  Event e =
      WaitForState(client, "manual", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Restart the subsystem
  status = client.RestartSubsystem("manual");
  ASSERT_TRUE(status.ok());
  sleep(1);
  WaitForState(client, "manual", AdminState::kOnline, OperState::kOnline);

  // Stop the subsystem
  status = client.StopSubsystem("manual");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "manual", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("manual", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, ManualRestartSimpleSubsystemAfterCrash) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "manual",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }},
       .restart_policy = adastra::capcom::client::RestartPolicy::kManual});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "manual", AdminState::kOffline, OperState::kOffline);

  status = client.StartSubsystem("manual");
  ASSERT_TRUE(status.ok());
  Event e =
      WaitForState(client, "manual", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Kill the process.
  SubsystemStatus s = std::get<0>(e.event);
  ASSERT_EQ(1, s.processes.size());
  int pid = s.processes[0].pid;
  kill(pid, SIGTERM);

  // Wait for degraded state.
  WaitForState(client, "manual", AdminState::kOnline, OperState::kDegraded);

  // Restart the subsystem
  status = client.RestartSubsystem("manual");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "manual", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Stop the subsystem
  status = client.StopSubsystem("manual");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "manual", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("manual", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, RestartProcessAfterCrash) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  absl::Status initStatus =
      client.Init(CapcomAddr(), "restartprocess",
                  adastra::kSubsystemStatusEvents | adastra::kAlarmEvents);
  ASSERT_TRUE(initStatus.ok());

  absl::Status status = client.AddSubsystem(
      "processonly",
      {.static_processes =
           {{
                .name = "loop1",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop2",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop3",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            }},
       .restart_policy = adastra::capcom::client::RestartPolicy::kProcessOnly});
  ASSERT_TRUE(status.ok());
  WaitForState(client, "processonly", AdminState::kOffline,
               OperState::kOffline);

  status = client.StartSubsystem("processonly");
  ASSERT_TRUE(status.ok());

  Event e = WaitForState(client, "processonly", AdminState::kOnline,
                         OperState::kOnline);
  sleep(1);

  // Kill loop3
  SubsystemStatus s = std::get<0>(e.event);
  ASSERT_EQ(3, s.processes.size());
  int pid = -1;
  for (auto &p : s.processes) {
    if (p.name == "loop3") {
      pid = p.pid;
      break;
    }
  }
  ASSERT_NE(-1, pid);
  kill(pid, SIGTERM);

  // The alarm will arrive before the subsystem goes into restarting.
  WaitForAlarm(client, adastra::Alarm::Type::kProcess,
               adastra::Alarm::Severity::kWarning,
               adastra::Alarm::Reason::kCrashed);

  // The process will be restarted and an alarm will be raised.
  // Wait for starting-processes state
  WaitForState(client, "processonly", AdminState::kOnline,
               OperState::kStartingProcesses);

  // The process will restart and we will be online again.
  WaitForState(client, "processonly", AdminState::kOnline, OperState::kOnline);

  // The alarm will have been cleared.
  auto alarms = client.GetAlarms();
  ASSERT_TRUE(alarms.ok());
  ASSERT_EQ(0, alarms->size());

  sleep(1);

  // Stop the subsystem
  status = client.StopSubsystem("processonly");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "processonly", AdminState::kOffline,
               OperState::kOffline);

  status = client.RemoveSubsystem("processonly", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, RestartProcessAfterCrashBackoff) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  absl::Status initStatus =
      client.Init(CapcomAddr(), "restartprocess",
                  adastra::kSubsystemStatusEvents | adastra::kAlarmEvents);
  ASSERT_TRUE(initStatus.ok());

  absl::Status status = client.AddSubsystem(
      "processonly",
      {.static_processes =
           {{
                .name = "loop1",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop2",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop3",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            }},
       .restart_policy = adastra::capcom::client::RestartPolicy::kProcessOnly});
  ASSERT_TRUE(status.ok());
  WaitForState(client, "processonly", AdminState::kOffline,
               OperState::kOffline);

  status = client.StartSubsystem("processonly");
  ASSERT_TRUE(status.ok());

  Event e = WaitForState(client, "processonly", AdminState::kOnline,
                         OperState::kOnline);
  sleep(1);

  for (int i = 0; i < 3; i++) {
    // Kill loop3
    SubsystemStatus s = std::get<0>(e.event);
    ASSERT_EQ(3, s.processes.size());
    int pid = -1;
    for (auto &p : s.processes) {
      if (p.name == "loop3") {
        pid = p.pid;
        break;
      }
    }
    ASSERT_NE(-1, pid);
    kill(pid, SIGTERM);

    // The alarm will arrive before the subsystem goes into restarting.
    WaitForAlarm(client, adastra::Alarm::Type::kProcess,
                 adastra::Alarm::Severity::kWarning,
                 adastra::Alarm::Reason::kCrashed);

    // The process will be restarted and an alarm will be raised.
    // Wait for starting-processes state
    WaitForState(client, "processonly", AdminState::kOnline,
                 OperState::kStartingProcesses);

    // The process will restart and we will be online again.
    e = WaitForState(client, "processonly", AdminState::kOnline,
                     OperState::kOnline);

    // The alarm will have been cleared.
    auto alarms = client.GetAlarms();
    ASSERT_TRUE(alarms.ok());
    ASSERT_EQ(0, alarms->size());

    sleep(1);
  }

  // Stop the subsystem
  status = client.StopSubsystem("processonly");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "processonly", AdminState::kOffline,
               OperState::kOffline);

  status = client.RemoveSubsystem("processonly", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, RestartProcessAfterCrashLimit) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  absl::Status initStatus =
      client.Init(CapcomAddr(), "restartprocess",
                  adastra::kSubsystemStatusEvents | adastra::kAlarmEvents);
  ASSERT_TRUE(initStatus.ok());

  absl::Status status = client.AddSubsystem(
      "processonly",
      {.static_processes =
           {{
                .name = "loop1",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop2",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop3",
                .executable = "${runfiles_dir}/_main/testdata/loop",
                .max_restarts = 2,
            }},
       .restart_policy = adastra::capcom::client::RestartPolicy::kProcessOnly});
  ASSERT_TRUE(status.ok());
  WaitForState(client, "processonly", AdminState::kOffline,
               OperState::kOffline);

  status = client.StartSubsystem("processonly");
  ASSERT_TRUE(status.ok());

  Event e = WaitForState(client, "processonly", AdminState::kOnline,
                         OperState::kOnline);
  sleep(1);

  for (int i = 0; i < 3; i++) {
    // Kill loop3
    SubsystemStatus s = std::get<0>(e.event);
    ASSERT_EQ(3, s.processes.size());
    int pid = -1;
    for (auto &p : s.processes) {
      if (p.name == "loop3") {
        pid = p.pid;
        break;
      }
    }
    ASSERT_NE(-1, pid);
    kill(pid, SIGTERM);

    // The alarm will arrive before the subsystem goes into restarting.
    if (i == 2) {
      // There is a max of 2 restarts, so this one will not restart and we will
      // get a different alarm.  The subsystem will go degraded.
      WaitForAlarm(client, adastra::Alarm::Type::kProcess,
                   adastra::Alarm::Severity::kCritical,
                   adastra::Alarm::Reason::kCrashed);
      WaitForState(client, "processonly", AdminState::kOnline,
                   OperState::kDegraded);
      break;
    }
    WaitForAlarm(client, adastra::Alarm::Type::kProcess,
                 adastra::Alarm::Severity::kWarning,
                 adastra::Alarm::Reason::kCrashed);

    // The process will be restarted and an alarm will be raised.
    // Wait for starting-processes state
    WaitForState(client, "processonly", AdminState::kOnline,
                 OperState::kStartingProcesses);

    // The process will restart and we will be online again.
    e = WaitForState(client, "processonly", AdminState::kOnline,
                     OperState::kOnline);

    // The alarm will have been cleared.
    auto alarms = client.GetAlarms();
    ASSERT_TRUE(alarms.ok());
    ASSERT_EQ(0, alarms->size());

    sleep(1);
  }

  // Stop the subsystem
  status = client.StopSubsystem("processonly");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "processonly", AdminState::kOffline,
               OperState::kOffline);

  status = client.RemoveSubsystem("processonly", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, RestartProcessAfterCrashLimitRecover) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  absl::Status initStatus =
      client.Init(CapcomAddr(), "restartprocess",
                  adastra::kSubsystemStatusEvents | adastra::kAlarmEvents);
  ASSERT_TRUE(initStatus.ok());

  absl::Status status = client.AddSubsystem(
      "processonly",
      {.static_processes =
           {{
                .name = "loop1",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop2",
                .executable = "${runfiles_dir}/_main/testdata/loop",
            },
            {
                .name = "loop3",
                .executable = "${runfiles_dir}/_main/testdata/loop",
                .max_restarts = 2,
            }},
       .restart_policy = adastra::capcom::client::RestartPolicy::kProcessOnly});
  ASSERT_TRUE(status.ok());
  WaitForState(client, "processonly", AdminState::kOffline,
               OperState::kOffline);

  status = client.StartSubsystem("processonly");
  ASSERT_TRUE(status.ok());

  Event e = WaitForState(client, "processonly", AdminState::kOnline,
                         OperState::kOnline);
  sleep(1);

  for (int i = 0; i < 3; i++) {
    // Kill loop3
    SubsystemStatus s = std::get<0>(e.event);
    ASSERT_EQ(3, s.processes.size());
    int pid = -1;
    for (auto &p : s.processes) {
      if (p.name == "loop3") {
        pid = p.pid;
        break;
      }
    }
    ASSERT_NE(-1, pid);
    kill(pid, SIGTERM);

    // The alarm will arrive before the subsystem goes into restarting.
    if (i == 2) {
      // There is a max of 2 restarts, so this one will not restart and we will
      // get a different alarm.  The subsystem will go degraded.
      WaitForAlarm(client, adastra::Alarm::Type::kProcess,
                   adastra::Alarm::Severity::kCritical,
                   adastra::Alarm::Reason::kCrashed);
      WaitForState(client, "processonly", AdminState::kOnline,
                   OperState::kDegraded);
      break;
    }
    WaitForAlarm(client, adastra::Alarm::Type::kProcess,
                 adastra::Alarm::Severity::kWarning,
                 adastra::Alarm::Reason::kCrashed);

    // The process will be restarted and an alarm will be raised.
    // Wait for starting-processes state
    WaitForState(client, "processonly", AdminState::kOnline,
                 OperState::kStartingProcesses);

    // The process will restart and we will be online again.
    e = WaitForState(client, "processonly", AdminState::kOnline,
                     OperState::kOnline);

    // The alarm will have been cleared.
    auto alarms = client.GetAlarms();
    ASSERT_TRUE(alarms.ok());
    ASSERT_EQ(0, alarms->size());

    sleep(1);
  }

  // Restart the loop3 process in the subsystem.
  status = client.RestartProcesses("processonly", {"loop3"});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "processonly", AdminState::kOnline, OperState::kOnline);

  // Stop the subsystem
  status = client.StopSubsystem("processonly");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "processonly", AdminState::kOffline,
               OperState::kOffline);

  status = client.RemoveSubsystem("processonly", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, Abort) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "subsys",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("subsys");
  ASSERT_TRUE(status.ok());

  sleep(1);

  status = client.Abort("Just because", false);
  ASSERT_TRUE(status.ok());

  WaitForState(client, "subsys", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("subsys", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, AbortThenGoAgain) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "subsys",
      {.static_processes = {{
           .name = "loop", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("subsys");
  ASSERT_TRUE(status.ok());

  sleep(1);

  status = client.Abort("Just because", false);
  ASSERT_TRUE(status.ok());

  WaitForState(client, "subsys", AdminState::kOffline, OperState::kOffline);

  status = client.StartSubsystem("subsys");
  ASSERT_TRUE(status.ok());

  sleep(1);

  status = client.StopSubsystem("subsys");
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("subsys", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, RestartSimpleSubsystemTree) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "child",
      {.static_processes = {{
           .name = "loop1", .executable = "${runfiles_dir}/_main/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "child", AdminState::kOffline, OperState::kOffline);

  status = client.AddSubsystem("parent", {.children = {
                                              "child",
                                          }});
  ASSERT_TRUE(status.ok());

  WaitForState(client, "parent", AdminState::kOffline, OperState::kOffline);

  status = client.StartSubsystem("parent");
  ASSERT_TRUE(status.ok());

  // Get the pid of the child process.
  Event e =
      WaitForState(client, "child", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  // Wait for parent to go online.
  WaitForState(client, "parent", AdminState::kOnline, OperState::kOnline);

  // Kill the child process.
  SubsystemStatus s = std::get<0>(e.event);
  ASSERT_EQ(1, s.processes.size());
  int pid = s.processes[0].pid;
  kill(pid, SIGTERM);

  // Wait for the subsytems to go into restarting, then back online.
  WaitForState(client, "child", AdminState::kOnline, OperState::kRestarting);
  WaitForState(client, "parent", AdminState::kOnline, OperState::kRestarting);

  WaitForState(client, "child", AdminState::kOnline, OperState::kOnline);
  WaitForState(client, "parent", AdminState::kOnline, OperState::kOnline);
  sleep(1);

  status = client.StopSubsystem("parent");
  ASSERT_TRUE(status.ok());
  WaitForState(client, "parent", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("child", false);
  std::cerr << status << std::endl;
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("parent", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, Zygote) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "zygote1",
      {.zygotes = {{
           .name = "loop",
           .executable =
               "${runfiles_dir}/_main/stagezero/zygote/standard_zygote",
       }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("zygote1");
  ASSERT_TRUE(status.ok());

  status = client.StopSubsystem("zygote1");
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("zygote1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, VirtualProcess) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  ASSERT_TRUE(client.SetParameter("/foo/bar", "baz").ok());

  absl::Status status = client.AddSubsystem(
      "zygote1",
      {.zygotes = {{
           .name = "zygote1",
           .executable =
               "${runfiles_dir}/_main/stagezero/zygote/standard_zygote",
       }}});
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "virtual", {.virtual_processes = {{
                      .name = "virtual_module",
                      .zygote = "zygote1",
                      .dso = "${runfiles_dir}/_main/testdata/module.so",
                      .main_func = "Main",
                      .notify = true,
                  }}});
  ASSERT_TRUE(status.ok());

  // Start zygote.
  status = client.StartSubsystem("zygote1");
  ASSERT_TRUE(status.ok());

  // Start virtual.
  status = client.StartSubsystem("virtual");
  ASSERT_TRUE(status.ok());

  sleep(1);

  // Stop virtual.
  status = client.StopSubsystem("virtual");
  ASSERT_TRUE(status.ok());

  // Stop zygote.
  status = client.StopSubsystem("zygote1");
  ASSERT_TRUE(status.ok());

  // Remove virtual and zygote.
  status = client.RemoveSubsystem("virtual", false);
  ASSERT_TRUE(status.ok());
  status = client.RemoveSubsystem("zygote1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, AbortVirtual) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "zygote1",
      {.zygotes = {{
           .name = "zygote1",
           .executable =
               "${runfiles_dir}/_main/stagezero/zygote/standard_zygote",
       }}});
  std::cerr << status.ToString() << std::endl;
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "virtual", {.virtual_processes = {{
                      .name = "virtual_module",
                      .zygote = "zygote1",
                      .dso = "${runfiles_dir}/_main/testdata/module.so",
                      .main_func = "Main",
                      .notify = false,
                  }}});
  ASSERT_TRUE(status.ok());

  // Start zygote.
  status = client.StartSubsystem("zygote1");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "zygote1", AdminState::kOnline, OperState::kOnline);

  // Start virtual.
  status = client.StartSubsystem("virtual");
  ASSERT_TRUE(status.ok());

  sleep(1);

  status = client.Abort("Just because", false);
  ASSERT_TRUE(status.ok());

  std::cerr << "WAITING FOR OFFLINE\n";
  WaitForState(client, "virtual", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("virtual", false);
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("zygote1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, TalkAndListen) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddGlobalVariable(
      {.name = "subspace_socket", .value = "/tmp/subspace", .exported = false});
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "subspace",
      {.static_processes = {{
           .name = "subspace_server",
           .executable = "${runfiles_dir}/_main/external/"
                         "_main~_repo_rules~subspace/server/"
                         "subspace_server",
           .args = {"--notify_fd=${notify_fd}"},
           .notify = true,
       }},
       .zygotes = {{
           .name = "standard_zygote",
           .executable =
               "${runfiles_dir}/_main/stagezero/zygote/standard_zygote",

       }}});
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "chat", {.virtual_processes =
                   {{
                        .name = "talker",
                        .zygote = "standard_zygote",
                        .dso = "${runfiles_dir}/_main/testdata/talker.so",
                        .main_func = "ModuleMain",
                    },
                    {
                        .name = "listener",
                        .zygote = "standard_zygote",
                        .dso = "${runfiles_dir}/_main/testdata/listener.so",
                        .main_func = "ModuleMain",
                    }},
               .children = {
                   "subspace",
               }});
  std::cerr << status.ToString() << std::endl;
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("subspace");
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("chat");
  ASSERT_TRUE(status.ok());
  sleep(2);

  status = client.StopSubsystem("chat");
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("chat", true);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, InteractiveEcho) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "echo", {
                  .static_processes = {{
                      .name = "echo",
                      .executable = "${runfiles_dir}/_main/testdata/echo",
                      .notify = true,
                      .interactive = true,
                  }},
              });
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem(
      "echo", adastra::capcom::client::RunMode::kInteractive);
  ASSERT_TRUE(status.ok());

  std::string data = WaitForOutput(client, "running");
  std::cout << "output: " << data;

  // Send a string to the echo program and check that it's echoed.
  SendInput(client, "echo", "echo", STDIN_FILENO, "testing\r\r\r\n");
  data = WaitForOutput(client, "testing");
  std::cout << "output: " << data;

  // Send a ^C to the process.
  SendInput(client, "echo", "echo", STDIN_FILENO, "\03");

  // The echo program prints "done" when it is exiting.
  data = WaitForOutput(client, "signal 2");
  std::cout << "output: " << data;

  WaitForState(client, "echo", AdminState::kOffline, OperState::kOffline);

  // When an interactive process ends it will close the client
  // socket, so we need to make a new one to remove the subsystem.

  adastra::capcom::client::Client client2(ClientMode::kNonBlocking);
  InitClient(client2, "foobar1");
  status = client2.RemoveSubsystem("echo", false);
  std::cerr << "remove " << status << std::endl;
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, NonInteractiveEcho) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "echo",
      {
          .static_processes = {{
              .name = "echo",
              .executable = "${runfiles_dir}/_main/testdata/echo",
              .notify = true,
              .oneshot = true,
          }},
          .streams = {{
                          .stream_fd = STDIN_FILENO,
                          .tty = true,
                          .disposition = adastra::Stream::Disposition::kClient,
                      },
                      {
                          .stream_fd = STDOUT_FILENO,
                          .tty = true,
                          .disposition = adastra::Stream::Disposition::kClient,
                      },
                      {
                          .stream_fd = STDERR_FILENO,
                          .tty = true,
                          .disposition = adastra::Stream::Disposition::kClient,
                          .direction = adastra::Stream::Direction::kOutput,
                      }},
      });
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("echo");
  ASSERT_TRUE(status.ok());

  std::stringstream current_output;
  WaitForState(client, "echo", AdminState::kOnline, OperState::kOnline,
               &current_output);

  std::cerr << "waiting for output\n";
  std::string currout = current_output.str();
  std::string data = WaitForOutput(client, "running", &currout);
  std::cout << "output: " << data;

  // Send a string to the echo program and check that it's echoed.
  SendInput(client, "echo", "echo", STDIN_FILENO, "testing\n");
  data = WaitForOutput(client, "testing");
  std::cout << "output: " << data;

  // Close the input stream.
  status = client.CloseFd("echo", "echo", STDIN_FILENO);
  std::cerr << "close " << status << std::endl;
  ASSERT_TRUE(status.ok());

  status = client.StopSubsystem("echo");
  ASSERT_TRUE(status.ok());
  
  WaitForState(client, "echo", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("echo", false);
  std::cerr << "remove " << status << std::endl;
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, BadStreams) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  // echo1 has stdin with output direction.
  absl::Status status = client.AddSubsystem(
      "echo1",
      {.static_processes = {{
           .name = "echo1",
           .executable = "${runfiles_dir}/_main/testdata/echo",
           .notify = true,
           .streams = {{
                           .stream_fd = STDIN_FILENO,
                           .tty = true,
                           .disposition = adastra::Stream::Disposition::kClient,
                           .direction = adastra::Stream::Direction::kOutput,
                       },
                       {
                           .stream_fd = STDOUT_FILENO,
                           .tty = true,
                           .disposition = adastra::Stream::Disposition::kClient,
                           .direction = adastra::Stream::Direction::kOutput,
                       },
                       {
                           .stream_fd = STDERR_FILENO,
                           .tty = true,
                           .disposition = adastra::Stream::Disposition::kClient,
                           .direction = adastra::Stream::Direction::kOutput,
                       }},
       }}});
  ASSERT_FALSE(status.ok());

  // echo2 has stdout as input
  status = client.AddSubsystem(
      "echo2",
      {.static_processes = {{
           .name = "echo2",
           .executable = "${runfiles_dir}/_main/testdata/echo",
           .notify = true,
           .streams = {{
                           .stream_fd = STDIN_FILENO,
                           .tty = true,
                           .disposition = adastra::Stream::Disposition::kClient,
                           .direction = adastra::Stream::Direction::kInput,
                       },
                       {
                           .stream_fd = STDOUT_FILENO,
                           .tty = true,
                           .disposition = adastra::Stream::Disposition::kClient,
                           .direction = adastra::Stream::Direction::kInput,
                       },
                       {
                           .stream_fd = STDERR_FILENO,
                           .tty = true,
                           .disposition = adastra::Stream::Disposition::kClient,
                           .direction = adastra::Stream::Direction::kOutput,
                       }},
       }}});
  ASSERT_FALSE(status.ok());

  // echo3 has default for stdin and stdout.
  status = client.AddSubsystem(
      "echo3",
      {.static_processes = {{
           .name = "echo3",
           .executable = "${runfiles_dir}/_main/testdata/echo",
           .notify = true,
           .streams = {{
                           .stream_fd = STDIN_FILENO,
                           .tty = true,
                           .disposition = adastra::Stream::Disposition::kClient,
                       },
                       {
                           .stream_fd = STDOUT_FILENO,
                           .tty = true,
                           .disposition = adastra::Stream::Disposition::kClient,
                       },
                       {
                           .stream_fd = STDERR_FILENO,
                           .tty = true,
                           .disposition = adastra::Stream::Disposition::kClient,
                           .direction = adastra::Stream::Direction::kOutput,
                       }},
       }}});
  ASSERT_TRUE(status.ok());

  // echo4 has a stream without a direction set.
  status = client.AddSubsystem(
      "echo4",
      {.static_processes = {{
           .name = "echo4",
           .executable = "${runfiles_dir}/_main/testdata/echo",
           .notify = true,
           .streams =
               {
                   {
                       .stream_fd = 4,
                       .tty = true,
                       .disposition = adastra::Stream::Disposition::kClient,
                   },
               },
       }}});
  ASSERT_FALSE(status.ok());

  status = client.RemoveSubsystem("echo3", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, CgroupOps) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  adastra::Cgroup cgroup = {.type = adastra::CgroupType::kDomain,
                            .name = "test"};
  cgroup.cpuset = std::make_unique<adastra::CgroupCpusetController>();
  cgroup.cpuset->cpus = "0:3";

  absl::Status status = client.AddCompute(
      "compute1", toolbelt::InetAddress("localhost", 6522),
      adastra::capcom::client::ComputeConnectionPolicy::kStatic,
      {
          cgroup,
      });
  ASSERT_TRUE(status.ok());

  status = client.FreezeCgroup("compute1", "test");
  ASSERT_TRUE(status.ok());

  status = client.ThawCgroup("compute1", "test");
  ASSERT_TRUE(status.ok());

  status = client.KillCgroup("compute1", "test");
  ASSERT_TRUE(status.ok());

  // Negative tests.
  status = client.FreezeCgroup("xxx", "test");
  ASSERT_FALSE(status.ok());

  status = client.ThawCgroup("xxx", "test");
  ASSERT_FALSE(status.ok());

  status = client.KillCgroup("xxx", "test");
  ASSERT_FALSE(status.ok());

  status = client.FreezeCgroup("compute1", "xxx");
  ASSERT_FALSE(status.ok());

  status = client.ThawCgroup("compute1", "xxx");
  ASSERT_FALSE(status.ok());

  status = client.KillCgroup("compute1", "xxx");
  ASSERT_FALSE(status.ok());

  status = client.RemoveCompute("compute1");
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, Parameters) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.DeleteParameters();
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/foo/bar", "value1");
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/foo/baz", "value2");
  ASSERT_TRUE(status.ok());

  // Add a subsystem that uses the parameters.
  status = client.AddSubsystem(
      "param", {
                   .static_processes = {{
                       .name = "params",
                       .executable = "${runfiles_dir}/_main/testdata/params",
                       .notify = true,
                   }},
               });
  // Start the subsystem
  status = client.StartSubsystem("param");
  ASSERT_TRUE(status.ok());

  std::cerr << "Waiting for parameter update\n";
  WaitForParameterUpdate(client, "/foo/bar", "global-foobar");

  std::cerr << "Waiting for parameter delete\n";
  WaitForParameterDelete(client, "/foo/baz");
  sleep(1);
  // Stop the subsystem.
  status = client.StopSubsystem("param");
  ASSERT_TRUE(status.ok());

  // Remove the subsystem.
  status = client.RemoveSubsystem("param", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, LocalParameters) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.DeleteParameters();
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/foo/bar", "global-value1");
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "param",
      {
          .static_processes = {{
              .name = "params",
              .executable = "${runfiles_dir}/_main/testdata/params",
              .parameters =
                  {
                      {"foo/bar", "local-value1"}, {"foo/baz", "local-value2"},
                  },
              .notify = true,
          }},
      });
  // Start the subsystem
  status = client.StartSubsystem("param");
  ASSERT_TRUE(status.ok());

  sleep(1);
  // Stop the subsystem.
  status = client.StopSubsystem("param");
  ASSERT_TRUE(status.ok());

  // Remove the subsystem.
  status = client.RemoveSubsystem("param", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, ParametersWithUpdate) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.DeleteParameters();
  ASSERT_TRUE(status.ok());
  status = client.SetParameter("/foo/bar", "value1");
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/foo/baz", "value2");
  ASSERT_TRUE(status.ok());

  // Add a subsystem that uses the parameters.
  status = client.AddSubsystem(
      "param", {
                   .static_processes = {{
                       .name = "params",
                       .executable = "${runfiles_dir}/_main/testdata/params",
                       .args = {"parameter_events"},
                       .notify = true,
                   }},
               });
  // Start the subsystem
  status = client.StartSubsystem("param");
  ASSERT_TRUE(status.ok());

  std::cerr << "Waiting for parameter update\n";
  WaitForParameterUpdate(client, "/foo/bar", "global-foobar");

  std::cerr << "Waiting for parameter delete\n";
  WaitForParameterDelete(client, "/foo/baz");
  sleep(1);
  // Stop the subsystem.
  status = client.StopSubsystem("param");
  ASSERT_TRUE(status.ok());

  // Remove the subsystem.
  status = client.RemoveSubsystem("param", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, LocalParametersWithEvents) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.DeleteParameters();
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/foo/bar", "global-value1");
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "param",
      {
          .static_processes = {{
              .name = "params",
              .executable = "${runfiles_dir}/_main/testdata/params",
              .parameters =
                  {
                      {"foo/bar", "local-value1"}, {"foo/baz", "local-value2"},
                  },
              .args = {"parameter_events"},
              .notify = true,
          }},
      });
  // Start the subsystem
  status = client.StartSubsystem("param");
  ASSERT_TRUE(status.ok());

  sleep(1);
  // Stop the subsystem.
  status = client.StopSubsystem("param");
  ASSERT_TRUE(status.ok());

  // Remove the subsystem.
  status = client.RemoveSubsystem("param", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, ParameterPerformance) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.DeleteParameters();
  ASSERT_TRUE(status.ok());

  // Add a lot of global parameters.
  for (int i = 0; i < 10; i++) {
    for (int j = 0; j < 10; j++) {
      for (int k = 0; k < 10; k++) {
        std::string name = absl::StrFormat("/%d/%d/%d", i, j, k);
        int32_t value = i * j * k;
        absl::Status status = client.SetParameter(name, value);
        ASSERT_TRUE(status.ok());
      }
    }
  }

  std::vector<adastra::parameters::Parameter> local_params;
  for (int i = 0; i < 5; i++) {
    for (int j = 0; j < 5; j++) {
      for (int k = 0; k < 5; k++) {
        std::string name = absl::StrFormat("%d/%d/%d", i, j, k);
        int32_t value = i * j * k;
        local_params.push_back({name, value});
      }
    }
  }
  status = client.AddSubsystem(
      "param", {
                   .static_processes = {{
                       .name = "params",
                       .executable = "${runfiles_dir}/_main/testdata/params",
                       .parameters = local_params,
                       .args = {"performance"},
                       .notify = true,
                   }},
               });
  // Start the subsystem
  status = client.StartSubsystem("param");
  ASSERT_TRUE(status.ok());

  sleep(1);
  // Stop the subsystem.
  status = client.StopSubsystem("param");
  ASSERT_TRUE(status.ok());

  // Remove the subsystem.
  status = client.RemoveSubsystem("param", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, AllParameterTypes) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.DeleteParameters();
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/parameter/int32", int32_t(1234));
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/parameter/int64", int64_t(4321));
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/parameter/string1", "string1");
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/parameter/string2", std::string("string2"));
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/parameter/double", 3.14159265);
  ASSERT_TRUE(status.ok());

  status = client.SetParameter("/parameter/bool", true);
  ASSERT_TRUE(status.ok());

  std::string bytes(1'000, 'a');
  status = client.SetParameter("/parameter/bytes",
                               absl::Span(bytes.data(), bytes.size()));
  ASSERT_TRUE(status.ok());

  struct timespec ts = {1234, 4321};
  status = client.SetParameter("/parameter/timespec", ts);
  ASSERT_TRUE(status.ok());

  std::vector<adastra::parameters::Value> values;
  values.push_back(int32_t(1234));
  values.push_back(int64_t(4321));
  values.push_back(std::string("string1"));

  status = client.SetParameter("/parameter/vector", values);
  ASSERT_TRUE(status.ok());

  std::map<std::string, adastra::parameters::Value> map;
  map["int32"] = int32_t(1234);
  map["int64"] = int64_t(4321);
  map["string"] = std::string("string1");

  status = client.SetParameter("/parameter/map", map);
  ASSERT_TRUE(status.ok());

  // Add a subsystem that uses the parameters.
  status = client.AddSubsystem(
      "param", {
                   .static_processes = {{
                       .name = "params",
                       .executable = "${runfiles_dir}/_main/testdata/params",
                       .notify = true,
                   }},
               });
  ASSERT_TRUE(status.ok());

  // Start the subsystem
  status = client.StartSubsystem("param");
  ASSERT_TRUE(status.ok());

  // Get all the parameters.
  auto params = client.GetParameters();
  ASSERT_TRUE(params.ok());
  ASSERT_EQ(12, params->size());

  // Print the parameters.
  for (auto &p : *params) {
    std::cerr << p.name << " = " << p.value << std::endl;
  }

  struct {
    std::string name;
    adastra::parameters::Value value;
  } expected_parameters[] = {
      {"/parameter/int32", int32_t(1234)},
      {"/parameter/int64", int64_t(4321)},
      {"/parameter/string1", "string1"},
      {"/parameter/string2", "string2"},
      {"/parameter/double", 3.14159265},
      {"/parameter/bool", true},
      {"/parameter/bytes", bytes},
      {"/parameter/timespec", ts},
      {"/parameter/vector", values},
      {"/parameter/map/int32", int32_t(1234)},
      {"/parameter/map/int64", int64_t(4321)},
      {"/parameter/map/string", "string1"},
  };
  // Check the parameters against the expected values.
  for (auto &p : expected_parameters) {
    std::cerr << "Checking " << p.name << std::endl;
    auto it = std::find_if(params->begin(), params->end(),
                           [&p](const adastra::parameters::Parameter &param) {
                             return param.name == p.name;
                           });
    ASSERT_NE(it, params->end());
    ASSERT_EQ(p.value, it->value);
  }

  // Get the value of each parameter individually and check its value.
  for (auto &p : expected_parameters) {
    auto value = client.GetParameter(p.name);
    ASSERT_TRUE(value.ok());
    ASSERT_EQ(p.value, *value);
  }

  // Check the map value.
  auto map_value = client.GetParameter("/parameter/map");
  ASSERT_TRUE(map_value.ok());
  ASSERT_EQ(adastra::parameters::Type::kMap, map_value->GetType());
  ASSERT_EQ(map, map_value->GetMap());

  sleep(1);
  // Stop the subsystem.
  status = client.StopSubsystem("param");
  ASSERT_TRUE(status.ok());

  // Remove the subsystem.
  status = client.RemoveSubsystem("param", false);
  ASSERT_TRUE(status.ok());
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  absl::ParseCommandLine(argc, argv);

  return RUN_ALL_TESTS();
}
