// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "capcom/capcom.h"
#include "capcom/client/client.h"
#include "capcom/subsystem.h"
#include "stagezero/stagezero.h"
#include <gtest/gtest.h>

#include <fstream>
#include <inttypes.h>
#include <memory>
#include <signal.h>
#include <sstream>
#include <sys/resource.h>
#include <thread>

ABSL_FLAG(bool, start_capcom, true, "Start capcom");

using AdminState = adastra::AdminState;
using OperState = adastra::OperState;
using SubsystemStatus = adastra::SubsystemStatus;
using Event = adastra::Event;
using EventType = adastra::EventType;
using ClientMode = adastra::capcom::client::ClientMode;

void SignalHandler(int sig);
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
    printf("Starting StageZero\n");

    // Capcom will write to this pipe to notify us when it
    // has started and stopped.  This end of the pipe is blocking.
    (void)pipe(stagezero_pipe_);

    stagezero_addr_ = toolbelt::InetAddress("localhost", port);
    stagezero_ = std::make_unique<adastra::stagezero::StageZero>(
        stagezero_scheduler_, stagezero_addr_, true, "/tmp",
        "debug",
        stagezero_pipe_[1]);

    // Start stagezero running in a thread.
    stagezero_thread_ = std::thread([]() {

      absl::Status s = stagezero_->Run();
    if (!s.ok()) {
        fprintf(stderr, "Error running stagezero: %s\n", s.ToString().c_str());
        exit(1);
      }
    });

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
        capcom_scheduler_, capcom_addr_, true, stagezero_port, "", "debug", false,
        capcom_pipe_[1]);

    // Start capcom running in a thread.
    capcom_thread_ = std::thread([]() {
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
    WaitForStop(capcom_pipe_[0], capcom_thread_);
  }

  static void StopStageZero() {
    printf("Stopping StageZero\n");
    stagezero_->Stop();

    // Wait for server to tell us that it's stopped.
    WaitForStop(stagezero_pipe_[0], stagezero_thread_);
  }

  static void WaitForStop(int fd, std::thread &t) {
    char buf[8];
    (void)::read(fd, buf, 8);
    t.join();
  }

  static void WaitForStop() {
    WaitForStop(stagezero_pipe_[0], stagezero_thread_);
    WaitForStop(capcom_pipe_[0], capcom_thread_);
  }

  void SetUp() override { signal(SIGPIPE, SIG_IGN); }
  void TearDown() override {}

  void InitClient(adastra::capcom::client::Client &client,
                  const std::string &name,
                  int event_mask = adastra::kSubsystemStatusEvents |
                                   adastra::kOutputEvents) {
    absl::Status s = client.Init(CapcomAddr(), name, event_mask);
    std::cout << "Init status: " << s << std::endl;
    ASSERT_TRUE(s.ok());
  }

  Event WaitForState(adastra::capcom::client::Client &client,
                     std::string subsystem, AdminState admin_state,
                     OperState oper_state) {
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
      }
    }
    EXPECT_TRUE(false);
    return {};
  }

  std::string WaitForOutput(adastra::capcom::client::Client &client,
                            std::string match) {
    std::cout << "waiting for output " << match << "\n";
    std::stringstream s;
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<adastra::Event>> e =
          client.WaitForEvent();
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
      absl::StatusOr<std::shared_ptr<adastra::Event>> e =
          client.WaitForEvent();
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

  void SendInput(adastra::capcom::client::Client &client,
                 std::string subsystem, std::string process, int fd,
                 std::string s) {
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
  static std::thread capcom_thread_;
  static toolbelt::InetAddress capcom_addr_;

  static co::CoroutineScheduler stagezero_scheduler_;
  static int stagezero_pipe_[2];
  static std::unique_ptr<adastra::stagezero::StageZero> stagezero_;
  static std::thread stagezero_thread_;
  static toolbelt::InetAddress stagezero_addr_;
};

co::CoroutineScheduler CapcomTest::capcom_scheduler_;
int CapcomTest::capcom_pipe_[2];
std::unique_ptr<adastra::capcom::Capcom> CapcomTest::capcom_;
std::thread CapcomTest::capcom_thread_;
toolbelt::InetAddress CapcomTest::capcom_addr_;

co::CoroutineScheduler CapcomTest::stagezero_scheduler_;
int CapcomTest::stagezero_pipe_[2];
std::unique_ptr<adastra::stagezero::StageZero> CapcomTest::stagezero_;
std::thread CapcomTest::stagezero_thread_;
toolbelt::InetAddress CapcomTest::stagezero_addr_;

void SignalHandler(int sig) {
  printf("Signal %d\n", sig);
  CapcomTest::Capcom()->Stop();
  CapcomTest::StageZero()->Stop();
  CapcomTest::WaitForStop();

  signal(sig, SIG_DFL);
  raise(sig);
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
      "foobar", {.static_processes = {{
                     .name = "loop",
                     .executable = "${runfiles_dir}/__main__/testdata/loop",
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
                     .executable = "${runfiles_dir}/__main__/testdata/loop",
                     .compute = "localhost",
                 }}});
  ASSERT_TRUE(status.ok());

  // Unknown compute should fail.
  status = client.AddSubsystem(
      "foobar2", {.static_processes = {{
                      .name = "loop",
                      .executable = "${runfiles_dir}/__main__/testdata/loop",
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
                         .executable = "${runfiles_dir}/__main__/testdata/loop",
                         .compute = "localhost1",
                     },
                     {
                         .name = "loop2",
                         .executable = "${runfiles_dir}/__main__/testdata/loop",
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
      "foobar1", {.static_processes = {{
                      .name = "loop",
                      .executable = "${runfiles_dir}/__main__/testdata/loop",
                  }}});
  ASSERT_TRUE(status.ok());

  status = client.StartSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  status = client.StopSubsystem("foobar1");
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("foobar1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, OneshotOK) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "oneshot", {.static_processes = {{
                      .name = "oneshot",
                      .executable = "${runfiles_dir}/__main__/testdata/oneshot",
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
                      .executable = "${runfiles_dir}/__main__/testdata/oneshot",
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
                      .executable = "${runfiles_dir}/__main__/testdata/oneshot",
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
                      .executable = "${runfiles_dir}/__main__/testdata/oneshot",
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
      "foobar1", {.static_processes = {{
                      .name = "loop",
                      .executable = "${runfiles_dir}/__main__/testdata/loop",
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
      "foobar1",
      {.static_processes = {
           {
               .name = "loop1",
               .executable = "${runfiles_dir}/__main__/testdata/loop",
               .compute = "localhost1",
           },
           {
               .name = "loop2",
               .executable = "${runfiles_dir}/__main__/testdata/loop",
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
      "foobar1", {.static_processes = {{
                      .name = "loop",
                      .executable = "${runfiles_dir}/__main__/testdata/loop",
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

TEST_F(CapcomTest, StartSimpleSubsystemTree) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "child", {.static_processes = {{
                    .name = "loop1",
                    .executable = "${runfiles_dir}/__main__/testdata/loop",
                }}});
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "parent", {.static_processes = {{
                     .name = "loop2",
                     .executable = "${runfiles_dir}/__main__/testdata/loop",
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
}

TEST_F(CapcomTest, Abort) {
  adastra::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "subsys", {.static_processes = {{
                     .name = "loop",
                     .executable = "${runfiles_dir}/__main__/testdata/loop",
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
      "subsys", {.static_processes = {{
                     .name = "loop",
                     .executable = "${runfiles_dir}/__main__/testdata/loop",
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
      "child", {.static_processes = {{
                    .name = "loop1",
                    .executable = "${runfiles_dir}/__main__/testdata/loop",
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
               "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote",
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

  absl::Status status = client.AddSubsystem(
      "zygote1",
      {.zygotes = {{
           .name = "zygote1",
           .executable =
               "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote",
       }}});
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "virtual", {.virtual_processes = {{
                      .name = "virtual_module",
                      .zygote = "zygote1",
                      .dso = "${runfiles_dir}/__main__/testdata/module.so",
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
               "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote",
       }}});
  std::cerr << status.ToString() << std::endl;
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "virtual", {.virtual_processes = {{
                      .name = "virtual_module",
                      .zygote = "zygote1",
                      .dso = "${runfiles_dir}/__main__/testdata/module.so",
                      .main_func = "Main",
                      .notify = false,
                  }}});
  ASSERT_TRUE(status.ok());

  // Start zygote.
  status = client.StartSubsystem("zygote1");
  ASSERT_TRUE(status.ok());

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
           .executable = "${runfiles_dir}/__main__/external/subspace/server/"
                         "subspace_server",
           .args = {"--notify_fd=${notify_fd}"},
           .notify = true,
       }},
       .zygotes = {{
           .name = "standard_zygote",
           .executable =
               "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote",

       }}});
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "chat", {.virtual_processes =
                   {{
                        .name = "talker",
                        .zygote = "standard_zygote",
                        .dso = "${runfiles_dir}/__main__/testdata/talker.so",
                        .main_func = "ModuleMain",
                    },
                    {
                        .name = "listener",
                        .zygote = "standard_zygote",
                        .dso = "${runfiles_dir}/__main__/testdata/listener.so",
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
                      .executable = "${runfiles_dir}/__main__/testdata/echo",
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

TEST_F(CapcomTest, BadStreams) {
  adastra::capcom::client::Client client(ClientMode::kNonBlocking);
  InitClient(client, "foobar1");

  // echo1 has stdin with output direction.
  absl::Status status = client.AddSubsystem(
      "echo1",
      {.static_processes = {{
           .name = "echo1",
           .executable = "${runfiles_dir}/__main__/testdata/echo",
           .notify = true,
           .streams =
               {{
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
           .executable = "${runfiles_dir}/__main__/testdata/echo",
           .notify = true,
           .streams =
               {{
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
           .executable = "${runfiles_dir}/__main__/testdata/echo",
           .notify = true,
           .streams =
               {{
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
           .executable = "${runfiles_dir}/__main__/testdata/echo",
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

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  absl::ParseCommandLine(argc, argv);

  return RUN_ALL_TESTS();
}