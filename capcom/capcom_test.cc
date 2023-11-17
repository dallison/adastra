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

using AdminState = stagezero::capcom::AdminState;
using OperState = stagezero::capcom::OperState;
using SubsystemStatusEvent = stagezero::capcom::client::SubsystemStatusEvent;
using Event = stagezero::capcom::client::Event;
using EventType = stagezero::capcom::client::EventType;
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
        capcom_scheduler_, capcom_addr_, capcom_pipe_[1]);

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
      absl::StatusOr<Event> event = client.WaitForEvent();
      EXPECT_TRUE(event.ok());
      if (!event.ok()) {
        std::cerr << event.status().ToString() << std::endl;
        return {};
      }
      if (event->type == EventType::kSubsystemStatus) {
        SubsystemStatusEvent &s = std::get<0>(event->event);
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
      "foobar",
      {.static_processes = {{
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
      "foobar",
      {.static_processes = {{
           .name = "loop",
           .executable = "${runfiles_dir}/__main__/testdata/loop",
           .compute = "localhost",
       }}});
  ASSERT_TRUE(status.ok());

  // Unknown compute should fail.
  status = client.AddSubsystem(
      "foobar2",
      {.static_processes = {{
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
      "foobar",
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
      "foobar1",
      {.static_processes = {{
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
      "foobar1",
      {.static_processes = {{
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
      "foobar1",
      {.static_processes = {{
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
  SubsystemStatusEvent s = std::get<0>(e.event);
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
      "child",
      {.static_processes = {{
           .name = "loop1",
           .executable = "${runfiles_dir}/__main__/testdata/loop",
       }}});
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "parent",
      {.static_processes = {{
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
      "subsys",
      {.static_processes = {{
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
      "subsys",
      {.static_processes = {{
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
      "child",
      {.static_processes = {{
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
  SubsystemStatusEvent s = std::get<0>(e.event);
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
           .executable = "${runfiles_dir}/__main__/testdata/zygote",
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
           .executable = "${runfiles_dir}/__main__/testdata/zygote",
       }}});
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "virtual",
      {.virtual_processes = {{
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
  stagezero::capcom::client::Client client(ClientMode::kBlocking);
  InitClient(client, "foobar1");

  absl::Status status = client.AddSubsystem(
      "zygote1",
      {.zygotes = {{
           .name = "zygote1",
           .executable = "${runfiles_dir}/__main__/testdata/zygote",
       }}});
  ASSERT_TRUE(status.ok());

  status = client.AddSubsystem(
      "virtual",
      {.virtual_processes = {{
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

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  absl::ParseCommandLine(argc, argv);

  return RUN_ALL_TESTS();
}