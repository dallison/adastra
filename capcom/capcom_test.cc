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

using AdminState = stagezero::AdminState;
using OperState = stagezero::OperState;
using SubsystemStatus = stagezero::SubsystemStatus;
using Event = stagezero::Event;
using EventType = stagezero::EventType;
using ClientMode = stagezero::capcom::client::ClientMode;

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

  static void StartStageZero() {
    printf("Starting StageZero\n");

    // Capcom will write to this pipe to notify us when it
    // has started and stopped.  This end of the pipe is blocking.
    (void)pipe(stagezero_pipe_);

    stagezero_addr_ = toolbelt::InetAddress("localhost", 6522);
    stagezero_ = std::make_unique<stagezero::StageZero>(
        stagezero_scheduler_, stagezero_addr_, stagezero_pipe_[1]);

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
    std::cout << "stagezero running\n";
    signal(SIGINT, SignalHandler);
    signal(SIGQUIT, SignalQuitHandler);
  }

  static void StartCapcom() {
    printf("Starting Capcom\n");

    // Capcom will write to this pipe to notify us when it
    // has started and stopped.  This end of the pipe is blocking.
    (void)pipe(capcom_pipe_);

    capcom_addr_ = toolbelt::InetAddress("localhost", 6523);
    capcom_ = std::make_unique<stagezero::capcom::Capcom>(
        capcom_scheduler_, capcom_addr_, "", capcom_pipe_[1]);

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

  void InitClient(stagezero::capcom::client::Client &client,
                  const std::string &name) {
    absl::Status s = client.Init(CapcomAddr(), name);
    std::cout << "Init status: " << s << std::endl;
    ASSERT_TRUE(s.ok());
  }

  Event WaitForState(stagezero::capcom::client::Client &client,
                     std::string subsystem, AdminState admin_state,
                     OperState oper_state) {
    std::cout << "waiting for subsystem state change " << subsystem << " "
              << admin_state << " " << oper_state << std::endl;
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<Event>> e = client.WaitForEvent();
      EXPECT_TRUE(e.ok());
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

  std::string WaitForOutput(stagezero::capcom::client::Client &client,
                            std::string match) {
    std::cout << "waiting for output " << match << "\n";
    std::stringstream s;
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<stagezero::Event>> e =
          client.WaitForEvent();
      std::cout << e.status().ToString() << "\n";
      if (!e.ok()) {
        return s.str();
      }
      std::shared_ptr<stagezero::Event> event = *e;
      if (event->type == stagezero::EventType::kOutput) {
        s << std::get<2>(event->event).data;
        if (s.str().find(match) != std::string::npos) {
          return s.str();
        }
      }
    }
    abort();
  }

  void SendInput(stagezero::capcom::client::Client &client,
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

  static stagezero::capcom::Capcom *Capcom() { return capcom_.get(); }
  static stagezero::StageZero *StageZero() { return stagezero_.get(); }

private:
  static co::CoroutineScheduler capcom_scheduler_;
  static int capcom_pipe_[2];
  static std::unique_ptr<stagezero::capcom::Capcom> capcom_;
  static std::thread capcom_thread_;
  static toolbelt::InetAddress capcom_addr_;

  static co::CoroutineScheduler stagezero_scheduler_;
  static int stagezero_pipe_[2];
  static std::unique_ptr<stagezero::StageZero> stagezero_;
  static std::thread stagezero_thread_;
  static toolbelt::InetAddress stagezero_addr_;
};

co::CoroutineScheduler CapcomTest::capcom_scheduler_;
int CapcomTest::capcom_pipe_[2];
std::unique_ptr<stagezero::capcom::Capcom> CapcomTest::capcom_;
std::thread CapcomTest::capcom_thread_;
toolbelt::InetAddress CapcomTest::capcom_addr_;

co::CoroutineScheduler CapcomTest::stagezero_scheduler_;
int CapcomTest::stagezero_pipe_[2];
std::unique_ptr<stagezero::StageZero> CapcomTest::stagezero_;
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
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");
}

TEST_F(CapcomTest, Compute) {
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "foobar", {.static_processes = {{
                     .name = "loop",
                     .executable = "${runfiles_dir}/__main__/testdata/loop",
                 }}});
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("foobar", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, SimpleSubsystemCompute) {
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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

TEST_F(CapcomTest, StartSimpleSubsystemCompute) {
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kNonBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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

  status = client.Abort("Just because");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "subsys", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("subsys", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, AbortThenGoAgain) {
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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

  status = client.Abort("Just because");
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
  stagezero::capcom::client::Client client(ClientMode::kNonBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kNonBlocking);
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
                  }}});
  ASSERT_TRUE(status.ok());

  // Start zygote.
  status = client.StartSubsystem("zygote1");
  ASSERT_TRUE(status.ok());

  // Start virtual.
  status = client.StartSubsystem("virtual");
  ASSERT_TRUE(status.ok());

  sleep(1);

  status = client.Abort("Just because");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "virtual", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("virtual", false);
  ASSERT_TRUE(status.ok());

  status = client.RemoveSubsystem("zygote1", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, TalkAndListen) {
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
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
  stagezero::capcom::client::Client client(ClientMode::kNonBlocking);
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
      "echo", stagezero::capcom::client::RunMode::kInteractive);
  ASSERT_TRUE(status.ok());

  std::string data = WaitForOutput(client, "running");
  std::cout << "output: " << data;

  // Send a string to the echo program and check that it's echoed.
  SendInput(client, "echo", "echo", STDIN_FILENO, "testing\r\r\r\n");
  data = WaitForOutput(client, "testing");
  std::cout << "output: " << data;

  // Close stdin for the process.  This will cause the process to exit.
  absl::Status close_status = client.CloseFd("echo", "echo", STDIN_FILENO);
  ASSERT_TRUE(close_status.ok());

  // The echo program prints "done" when it is exiting.
  data = WaitForOutput(client, "done");
  std::cout << "output: " << data;

  status = client.StopSubsystem("echo");
  ASSERT_TRUE(status.ok());

  WaitForState(client, "echo", AdminState::kOffline, OperState::kOffline);

  status = client.RemoveSubsystem("echo", false);
  ASSERT_TRUE(status.ok());
}

TEST_F(CapcomTest, BadStreams) {
  stagezero::capcom::client::Client client(ClientMode::kNonBlocking);
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
                    .disposition = stagezero::Stream::Disposition::kClient,
                    .direction = stagezero::Stream::Direction::kOutput,
                },
                {
                    .stream_fd = STDOUT_FILENO,
                    .tty = true,
                    .disposition = stagezero::Stream::Disposition::kClient,
                    .direction = stagezero::Stream::Direction::kOutput,
                },
                {
                    .stream_fd = STDERR_FILENO,
                    .tty = true,
                    .disposition = stagezero::Stream::Disposition::kClient,
                    .direction = stagezero::Stream::Direction::kOutput,
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
                    .disposition = stagezero::Stream::Disposition::kClient,
                    .direction = stagezero::Stream::Direction::kInput,
                },
                {
                    .stream_fd = STDOUT_FILENO,
                    .tty = true,
                    .disposition = stagezero::Stream::Disposition::kClient,
                    .direction = stagezero::Stream::Direction::kInput,
                },
                {
                    .stream_fd = STDERR_FILENO,
                    .tty = true,
                    .disposition = stagezero::Stream::Disposition::kClient,
                    .direction = stagezero::Stream::Direction::kOutput,
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
                    .disposition = stagezero::Stream::Disposition::kClient,
                },
                {
                    .stream_fd = STDOUT_FILENO,
                    .tty = true,
                    .disposition = stagezero::Stream::Disposition::kClient,
                },
                {
                    .stream_fd = STDERR_FILENO,
                    .tty = true,
                    .disposition = stagezero::Stream::Disposition::kClient,
                    .direction = stagezero::Stream::Direction::kOutput,
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
                       .disposition = stagezero::Stream::Disposition::kClient,
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