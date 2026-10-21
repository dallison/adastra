// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_split.h"

#include "stagezero/client/client.h"
#include "stagezero/stagezero.h"
#include <gtest/gtest.h>

#include "testdata/proto/telemetry.pb.h"
#include <fstream>
#include <grp.h>
#include <inttypes.h>
#include <linux/capability.h>
#include <pwd.h>
#include <memory>
#include <signal.h>
#include <sstream>
#include <sys/resource.h>
#include <thread>

ABSL_FLAG(bool, start_server, true, "Start the stagezero server");

void SignalHandler(int sig);

class ClientTest : public ::testing::Test {
public:
  // We run one server for the duration of the whole test suite.
  static void SetUpTestSuite() {
    if (!absl::GetFlag(FLAGS_start_server)) {
      return;
    }

    printf("Starting StageZero server\n");

    // The server will write to this pipe to notify us when it
    // has started and stopped.  This end of the pipe is blocking.
    (void)pipe(server_pipe_);

    addr_ = toolbelt::InetAddress("localhost", 6522);
    server_ = std::make_unique<adastra::stagezero::StageZero>(
        scheduler_, addr_, true, "/tmp", "debug", "", server_pipe_[1]);

    // Start server running in a thread.
    server_thread_ = std::thread([]() {
      absl::Status s = server_->Run();
      if (!s.ok()) {
        fprintf(stderr, "Error running StageZero server: %s\n",
                s.ToString().c_str());
        exit(1);
      }
    });

    // Wait for server to tell us that it's running.
    char buf[8];
    (void)::read(server_pipe_[0], buf, 8);
    std::cout << "server running\n";

    signal(SIGINT, SignalHandler);
    signal(SIGQUIT, SignalHandler);
  }

  static void TearDownTestSuite() {
    if (!absl::GetFlag(FLAGS_start_server)) {
      return;
    }
    std::cerr << "Stopping StageZero server\n";
    server_->Stop();

    // Wait for server to tell us that it's stopped.
    WaitForServerStop();
  }

  static void WaitForServerStop() {
    char buf[8];
    (void)::read(server_pipe_[0], buf, 8);
    server_thread_.join();
  }

  void SetUp() override { signal(SIGPIPE, SIG_IGN); }
  void TearDown() override {}

  void InitClient(adastra::stagezero::Client &client, const std::string &name,
                  int event_mask = adastra::kNoEvents) {
    absl::Status s = client.Init(Addr(), name, event_mask);
    std::cout << "Init status: " << s << std::endl;
    ASSERT_TRUE(s.ok());
    WaitForEvent(client, adastra::stagezero::control::Event::kConnect);
  }

  void WaitForEvent(adastra::stagezero::Client &client,
                    adastra::stagezero::control::Event::EventCase type,
                    bool allow_disconnect = false) {
    std::cout << "waiting for event " << type << std::endl;
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
          client.WaitForEvent();
      std::cout << e.status().ToString() << "\n";
      ASSERT_TRUE(e.ok());
      std::shared_ptr<adastra::stagezero::control::Event> event = *e;
      if (event->event_case() == adastra::stagezero::control::Event::kOutput) {
        // Ignore output events.
        continue;
      }
      if (type != adastra::stagezero::control::Event::kTelemetry &&
          event->event_case() ==
              adastra::stagezero::control::Event::kTelemetry) {
        // Telemetry processes send periodic status events.
        retry--;
        continue;
      }
      if (allow_disconnect &&
          type == adastra::stagezero::control::Event::kStop &&
          event->event_case() ==
              adastra::stagezero::control::Event::kDisconnect) {
        std::cerr << "Disconnected while waiting for stop, OK\n";
        return;
      }
      ASSERT_EQ(type, event->event_case());
      std::cout << event->DebugString();
      return;
    }
    FAIL();
  }

  std::string WaitForOutput(adastra::stagezero::Client &client,
                            std::string match, bool *got_stop = nullptr,
                            int retries = 10) {
    std::cout << "waiting for output " << match << "\n";
    std::stringstream s;
    for (int retry = 0; retry < retries; retry++) {
      absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
          client.WaitForEvent();
      std::cout << e.status().ToString() << "\n";
      if (!e.ok()) {
        return s.str();
      }
      std::shared_ptr<adastra::stagezero::control::Event> event = *e;
      std::cout << event->DebugString();
      if (event->event_case() == adastra::stagezero::control::Event::kStop) {
        if (got_stop != nullptr)
          *got_stop = true;
      }
      if (event->event_case() == adastra::stagezero::control::Event::kOutput) {
        s << event->output().data();
        if (s.str().find(match) != std::string::npos) {
          return s.str();
        }
      }
    }
    abort();
  }

  std::unique_ptr<adastra::stagezero::control::TelemetryEvent>
  WaitForTelemetryEvent(adastra::stagezero::Client &client) {
    std::cout << "waiting for telemetry event " << std::endl;
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
          client.WaitForEvent();
      std::cout << e.status().ToString() << "\n";
      EXPECT_TRUE(e.ok());
      std::shared_ptr<adastra::stagezero::control::Event> event = *e;
      if (event->event_case() == adastra::stagezero::control::Event::kOutput) {
        // Ignore output events.
        continue;
      }
      if (event->event_case() ==
          adastra::stagezero::control::Event::kTelemetry) {
        std::cerr << "Got telemetry event " << event->DebugString()
                  << std::endl;
        return std::make_unique<adastra::stagezero::control::TelemetryEvent>(
            event->telemetry());
      }
      std::cerr << "Waiting for telemetry, got " << event->DebugString()
                << std::endl;
      return nullptr;
    }
    EXPECT_TRUE(false);
    return nullptr;
  }

  void SendInput(adastra::stagezero::Client &client, std::string process_id,
                 int fd, std::string s) {
    absl::Status status = client.SendInput(process_id, fd, s);
    ASSERT_TRUE(status.ok());
  }

  static const toolbelt::InetAddress &Addr() { return addr_; }

  co::CoroutineScheduler &Scheduler() const { return scheduler_; }

  static adastra::stagezero::StageZero *Server() { return server_.get(); }

private:
  static co::CoroutineScheduler scheduler_;
  static std::string socket_;
  static int server_pipe_[2];
  static std::unique_ptr<adastra::stagezero::StageZero> server_;
  static std::thread server_thread_;
  static toolbelt::InetAddress addr_;
};

co::CoroutineScheduler ClientTest::scheduler_;
int ClientTest::server_pipe_[2];
std::unique_ptr<adastra::stagezero::StageZero> ClientTest::server_;
std::thread ClientTest::server_thread_;
toolbelt::InetAddress ClientTest::addr_;

void SignalHandler(int sig) {
  if (sig == SIGQUIT) {
    ClientTest::Server()->ShowCoroutines();
  }
  printf("Signal %d\n", sig);
  ClientTest::Server()->Stop();
  ClientTest::WaitForServerStop();

  signal(sig, SIG_DFL);
  raise(sig);
}

TEST_F(ClientTest, Init) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar1");
}

TEST_F(ClientTest, LaunchAndStop) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop", "${runfiles_dir}/_main/testdata/loop",
                                 {
                                     .args =
                                         {
                                             "ignore_signal",
                                         },
                                     .notify = true,
                                 });
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopSigTerm) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop", "${runfiles_dir}/_main/testdata/loop",
                                 {
                                     .args =
                                         {
                                             "ignore_signal",
                                         },
                                     .sigint_shutdown_timeout_secs = 0,
                                     .sigterm_shutdown_timeout_secs = 1,
                                     .notify = true,
                                 });
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopSigKill) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop", "${runfiles_dir}/_main/testdata/loop",
                                 {
                                     .args =
                                         {
                                             "ignore_signal",
                                         },
                                     .sigint_shutdown_timeout_secs = 0,
                                     .sigterm_shutdown_timeout_secs = 0,
                                     .notify = true,
                                 });
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchExitBeforeNotify) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop", "${runfiles_dir}/_main/testdata/loop",
                                 {
                                     .args =
                                         {
                                             "exit_before_notify",
                                         },
                                     .startup_timeout_secs = 100,
                                     .notify = true,
                                 });
  ASSERT_TRUE(status.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopTelemetry) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("telemetry",
                                 "${runfiles_dir}/_main/testdata/telemetry",
                                 {
                                     .notify = true, .telemetry = true,
                                 });
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopNoTelemetryTimeout) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("telemetry",
                                 "${runfiles_dir}/_main/testdata/telemetry",
                                 {
                                     .telemetry_shutdown_timeout_secs = 0,
                                     .notify = true,
                                     .telemetry = true,
                                 });
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopTelemetryCommand) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("telemetry",
                                 "${runfiles_dir}/_main/testdata/telemetry",
                                 {
                                     .notify = true, .telemetry = true,
                                 });
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  stagezero::ShutdownCommand cmd(1, 2);
  absl::Status s = client.SendTelemetryCommand(process_id, cmd);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
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

TEST_F(ClientTest, LaunchAndStopCustomTelemetryCommand) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2", adastra::kTelemetryEvents);

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("telemetry",
                                 "${runfiles_dir}/_main/testdata/telemetry",
                                 {
                                     .notify = true, .telemetry = true,
                                 });
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  // Send a custom telemetry command to get the PID.
  PidCommand cmd;
  absl::Status s = client.SendTelemetryCommand(process_id, cmd);

  auto ts_event = WaitForTelemetryEvent(client);
  if (ts_event->telemetry()
          .status()
          .Is<::testdata::telemetry::PidTelemetryStatus>()) {
    ::testdata::telemetry::PidTelemetryStatus status;
    auto ok = ts_event->telemetry().status().UnpackTo(&status);
    ASSERT_TRUE(ok);
    std::cout << "PID: " << status.pid() << std::endl;
  }
  s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop,
               /*allow_disconnect=*/true);
}

TEST_F(ClientTest, LaunchAndStopCustomTelemetryStatus) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2", adastra::kTelemetryEvents);

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("telemetry",
                                 "${runfiles_dir}/_main/testdata/telemetry",
                                 {
                                     .notify = true, .telemetry = true,
                                 });
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  for (int i = 0; i < 5; i++) {
    auto ts_event = WaitForTelemetryEvent(client);
    if (ts_event->telemetry()
            .status()
            .Is<::testdata::telemetry::TimeTelemetryStatus>()) {
      ::testdata::telemetry::TimeTelemetryStatus status;
      auto ok = ts_event->telemetry().status().UnpackTo(&status);
      ASSERT_TRUE(ok);
      std::cout << "Time: " << status.time() << std::endl;
    }
  }

  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop,
               /*allow_disconnect=*/true);
}

TEST_F(ClientTest, LaunchAndKill) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop", "${runfiles_dir}/_main/testdata/loop",
                                 {
                                     .notify = true,
                                 });
  ASSERT_TRUE(status.ok());
  int pid = status->second;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::cout << "killing process " << pid << std::endl;
  kill(pid, SIGTERM);
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopNoNotify) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop", "${runfiles_dir}/_main/testdata/loop",
                                 {.args = {
                                      "ignore_signal",
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  sleep(1);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopRepeated) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  for (int i = 0; i < 10; i++) {
    absl::StatusOr<std::pair<std::string, int>> status =
        client.LaunchStaticProcess(
            "loop", "${runfiles_dir}/_main/testdata/loop",
            {
                .startup_timeout_secs = 1, .notify = true,
            });
    ASSERT_TRUE(status.ok());
    std::string process_id = status->first;
    WaitForEvent(client, adastra::stagezero::control::Event::kStart);
    std::cout << "stopping process " << process_id << std::endl;
    absl::Status s = client.StopProcess(process_id);
    ASSERT_TRUE(s.ok());
    WaitForEvent(client, adastra::stagezero::control::Event::kStop);
  }
}

TEST_F(ClientTest, LaunchAndStopRepeatedNewClient) {
  for (int i = 0; i < 10; i++) {
    adastra::stagezero::Client client;
    InitClient(client, "foobar2");

    absl::StatusOr<std::pair<std::string, int>> status =
        client.LaunchStaticProcess("loop",
                                   "${runfiles_dir}/_main/testdata/loop",
                                   {
                                       .notify = true,
                                   });
    ASSERT_TRUE(status.ok());
    std::string process_id = status->first;
    WaitForEvent(client, adastra::stagezero::control::Event::kStart);
    std::cout << "stopping process " << process_id << std::endl;
    absl::Status s = client.StopProcess(process_id);
    ASSERT_TRUE(s.ok());
    WaitForEvent(client, adastra::stagezero::control::Event::kStop);
  }
}

TEST_F(ClientTest, Output) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2", adastra::kOutputEvents);

  adastra::Stream output = {
      .stream_fd = 1,
      .disposition = adastra::Stream::Disposition::kClient,
      .direction = adastra::Stream::Direction::kOutput,
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop", "${runfiles_dir}/_main/testdata/loop",
                                 {.streams = {
                                      output,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::string data = WaitForOutput(client, "loop 2");
  std::cout << data;

  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
  // sleep(2);
  // std::cout << "sleep done" << std::endl;
}

TEST_F(ClientTest, OutputTTY) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2", adastra::kOutputEvents);

  adastra::Stream output = {
      .stream_fd = 1,
      .tty = true,
      .disposition = adastra::Stream::Disposition::kClient,
      .direction = adastra::Stream::Direction::kOutput,
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop2", "${runfiles_dir}/_main/testdata/loop",
                                 {.streams = {
                                      output,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::string data = WaitForOutput(client, "loop 2");
  std::cout << data;

  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, OutputSyslog) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2", adastra::kOutputEvents);

  adastra::Stream output = {
      .stream_fd = 1,
      .tty = true,
      .disposition = adastra::Stream::Disposition::kSyslog,
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop2", "${runfiles_dir}/_main/testdata/loop",
                                 {.streams = {
                                      output,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  sleep(2);

  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, Echo) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2", adastra::kOutputEvents);

  adastra::Stream output = {
      .stream_fd = 1,
      .disposition = adastra::Stream::Disposition::kClient,
      .direction = adastra::Stream::Direction::kOutput,
  };

  adastra::Stream input = {
      .stream_fd = 0,
      .disposition = adastra::Stream::Disposition::kClient,
      .direction = adastra::Stream::Direction::kInput,
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("echo", "${runfiles_dir}/_main/testdata/echo",
                                 {.streams = {
                                      output, input,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;

  // The echo program prints "running" when it starts.
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::string data = WaitForOutput(client, "running");
  std::cout << "output: " << data;

  // Send a string to the echo program and check that it's echoed.
  SendInput(client, process_id, 0, "testing\n");
  data = WaitForOutput(client, "testing");
  std::cout << "output: " << data;

  // Close stdin for the process.  This will cause the process to exit.
  absl::Status close_status = client.CloseProcessFileDescriptor(process_id, 0);
  ASSERT_TRUE(close_status.ok());

  // The echo program prints "done" when it is exiting.
  data = WaitForOutput(client, "done");
  std::cout << "output: " << data;

  // Wait for it to stop.
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, EchoFileRead) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2", adastra::kOutputEvents);

  adastra::Stream output = {
      .stream_fd = 1,
      .disposition = adastra::Stream::Disposition::kClient,
      .direction = adastra::Stream::Direction::kOutput,
  };

  adastra::Stream input = {
      .stream_fd = 0,
      .disposition = adastra::Stream::Disposition::kFile,
      .direction = adastra::Stream::Direction::kInput,
      .data =
          {
              "${runfiles_dir}/_main/testdata/input_data.txt",
          },
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("echo", "${runfiles_dir}/_main/testdata/echo",
                                 {.streams = {
                                      output, input,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;

  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  std::string data = WaitForOutput(client, "from a file\ndone\n");
  std::cout << "output: " << data;

  // Stop the process.
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, EchoFileWrite) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  adastra::Stream output = {
      .stream_fd = 1,
      .disposition = adastra::Stream::Disposition::kFile,
      .direction = adastra::Stream::Direction::kOutput,
      .data =
          {
              "/tmp/echo.txt",
          },
  };

  adastra::Stream input = {
      .stream_fd = 0,
      .disposition = adastra::Stream::Disposition::kFile,
      .direction = adastra::Stream::Direction::kInput,
      .data =
          {
              "${runfiles_dir}/_main/testdata/input_data.txt",
          },
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("echo", "${runfiles_dir}/_main/testdata/echo",
                                 {.streams = {
                                      output, input,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;

  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  // Process will exit itself.
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);

  std::ifstream in("/tmp/echo.txt");
  ASSERT_TRUE(in);

  std::string expected = R"(running
this input is
from a file
done
)";
  char buf[256];
  in.read(buf, sizeof(buf));
  std::streamsize n = in.gcount();
  ASSERT_GE(n, 0);
  buf[n] = '\0';
  ASSERT_EQ(expected, buf);
}

TEST_F(ClientTest, Vars) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar4");

  absl::Status var_status = client.SetGlobalVariable("foobar", "barfoo", false);
  ASSERT_TRUE(var_status.ok());

  var_status = client.SetGlobalVariable("FOOBAR", "BARFOO", true);
  ASSERT_TRUE(var_status.ok());

  absl::StatusOr<std::pair<std::string, bool>> var_status2 =
      client.GetGlobalVariable("foobar");
  ASSERT_TRUE(var_status2.ok());
  ASSERT_EQ("barfoo", var_status2->first);
  ASSERT_FALSE(var_status2->second); // Not exported.

  var_status2 = client.GetGlobalVariable("FOOBAR");
  ASSERT_TRUE(var_status2.ok());
  ASSERT_EQ("BARFOO", var_status2->first);
  ASSERT_TRUE(var_status2->second); // Exported.
}

TEST_F(ClientTest, ProcessVars) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar4", adastra::kOutputEvents);

  absl::Status var_status = client.SetGlobalVariable("foobar", "barfoo", false);
  ASSERT_TRUE(var_status.ok());

  var_status = client.SetGlobalVariable("FOOBAR", "BARFOO", true);
  ASSERT_TRUE(var_status.ok());

  absl::StatusOr<std::pair<std::string, bool>> var_status2 =
      client.GetGlobalVariable("foobar");
  ASSERT_TRUE(var_status2.ok());
  ASSERT_EQ("barfoo", var_status2->first);
  ASSERT_FALSE(var_status2->second); // Not exported.

  var_status2 = client.GetGlobalVariable("FOOBAR");
  ASSERT_TRUE(var_status2.ok());
  ASSERT_EQ("BARFOO", var_status2->first);
  ASSERT_TRUE(var_status2->second); // Exported.

  var_status2 = client.GetGlobalVariable("runfiles_dir");
  ASSERT_TRUE(var_status2.ok());
  std::string runfiles = std::move(var_status2->first);

  // Start the 'vars' process to print args and env.

  adastra::Stream output = {
      .stream_fd = 1,
      .disposition = adastra::Stream::Disposition::kClient,
      .direction = adastra::Stream::Direction::kOutput,
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess(
          "vars", "${runfiles_dir}/_main/testdata/${procname}",
          {.vars =
               {
                   {.name = "dave", .value = "allison", .exported = false},
                   {.name = "DAVE", .value = "ALLISON", .exported = true},
                   {.name = "procname", .value = "vars", .exported = false},

               },
           .args =
               {
                   "${dave}123", "$foobar 456",
               },
           .streams =
               {
                   output,
               },
           // Don't notify because then there's a race between the output
           // events and the startup event and this test needs them in a
           // specific order.
           .notify = false});
  ASSERT_TRUE(status.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  bool got_stop = false;
  std::string data = WaitForOutput(client, "DONE\n", &got_stop);
  size_t fds = data.find("STAGEZERO_PARAMETERS_FDS=");
  if (fds != std::string::npos) {
    size_t nl = data.find('\n', fds);
    if (nl != std::string::npos) {
      // Go back to an equals sign.
      size_t eq = data.rfind('=', nl);
      if (eq != std::string::npos) {
        data.replace(eq + 1, nl - eq - 1, "X:X:X");
      }
    }
  }
  std::string expected = "arg[0]: " + runfiles +
                         R"(/_main/testdata/vars
arg[1]: allison123
arg[2]: barfoo 456
DAVE=ALLISON
FOOBAR=BARFOO
STAGEZERO_PARAMETERS_FDS=X:X:X
DONE
)";
  ASSERT_EQ(expected, data);
  std::cerr << "waiting for stop\n";
  if (!got_stop) {
    WaitForEvent(client, adastra::stagezero::control::Event::kStop);
  }
}

TEST_F(ClientTest, LaunchZygoteAndStop) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status = client.LaunchZygote(
      "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(status.ok());

  std::string process_id = status->first;
  printf("process id: %s\n", process_id.c_str());
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchFailureNamesWhatWasLaunched) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  // Zygotes and static processes share a launch path, so the error must say
  // which one failed.
  absl::StatusOr<std::pair<std::string, int>> zygote = client.LaunchZygote(
      "missing_zygote", "${runfiles_dir}/_main/no_such_zygote");
  ASSERT_FALSE(zygote.ok());
  EXPECT_NE(zygote.status().ToString().find(
                "Failed to launch zygote 'missing_zygote'"),
            std::string::npos)
      << zygote.status();

  absl::StatusOr<std::pair<std::string, int>> process =
      client.LaunchStaticProcess("missing_process",
                                 "${runfiles_dir}/_main/no_such_process");
  ASSERT_FALSE(process.ok());
  EXPECT_NE(process.status().ToString().find(
                "Failed to launch static process 'missing_process'"),
            std::string::npos)
      << process.status();
}

TEST_F(ClientTest, LaunchedProcessesDoNotInheritStageZeroState) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2", adastra::kOutputEvents);

  struct ProbeResult {
    int sockets = -1;
    int eventfds = -1;
    int sigpipe_ignored = -1;
  };
  auto run_probe = [this, &client](const std::string &name) {
    ProbeResult result;
    adastra::Stream output = {
        .stream_fd = 1,
        .disposition = adastra::Stream::Disposition::kClient,
        .direction = adastra::Stream::Direction::kOutput,
    };
    absl::StatusOr<std::pair<std::string, int>> status =
        client.LaunchStaticProcess(
            name, "${runfiles_dir}/_main/testdata/probe", {.streams = {output}});
    if (!status.ok()) {
      ADD_FAILURE() << status.status();
      return result;
    }
    bool got_stop = false;
    std::string data = WaitForOutput(client, "\n", &got_stop);
    if (!got_stop) {
      WaitForEvent(client, adastra::stagezero::control::Event::kStop);
    }
    std::istringstream s(data);
    if (!(s >> result.sockets >> result.eventfds >> result.sigpipe_ignored)) {
      ADD_FAILURE() << "Invalid probe output: " << data;
    }
    return result;
  };

  ProbeResult before = run_probe("probe_before_zygote");
  // StageZero ignores SIGPIPE; its processes must not.
  EXPECT_EQ(0, before.sigpipe_ignored);

  absl::StatusOr<std::pair<std::string, int>> zygote = client.LaunchZygote(
      "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote.ok()) << zygote.status();
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  // The zygote's control sockets must not leak into other processes.
  ProbeResult after = run_probe("probe_after_zygote");
  ASSERT_GE(before.sockets, 0);
  EXPECT_EQ(before.sockets, after.sockets);
  EXPECT_EQ(before.eventfds, after.eventfds);

  ASSERT_TRUE(client.StopProcess(zygote->first).ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchZygoteAndKill) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status = client.LaunchZygote(
      "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(status.ok());

  int pid = status->second;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::cout << "killing process " << pid << std::endl;
  kill(pid, SIGTERM);
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, Launch2ZygotesAndStop) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  std::vector<std::string> process_ids;
  for (int i = 0; i < 2; i++) {
    absl::StatusOr<std::pair<std::string, int>> status = client.LaunchZygote(
        absl::StrFormat("zygote_%d", i),
        "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
    ASSERT_TRUE(status.ok());

    std::string process_id = status->first;
    process_ids.push_back(process_id);
    WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  }
  for (auto &process_id : process_ids) {
    std::cout << "stopping process " << process_id << std::endl;
    absl::Status s = client.StopProcess(process_id);
    ASSERT_TRUE(s.ok());
    WaitForEvent(client, adastra::stagezero::control::Event::kStop);
  }
}

TEST_F(ClientTest, LaunchAndStopVirtual) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  // Launch zygote.
  absl::StatusOr<std::pair<std::string, int>> zygote_status =
      client.LaunchZygote(
          "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote_status.ok());

  std::string zygote_process_id = zygote_status->first;
  printf("zygote process id: %s\n", zygote_process_id.c_str());
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  absl::StatusOr<std::pair<std::string, int>> virt_status =
      client.LaunchVirtualProcess("modtest", "zygote",
                                  "${runfiles_dir}/_main/"
                                  "testdata/module.so",
                                  "Main",
                                  {.args = {"ignore_signal"}, .notify = true});
  ASSERT_TRUE(virt_status.ok());
  std::string virt_process_id = virt_status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  absl::Status s = client.StopProcess(virt_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);

  // Stop zygote
  s = client.StopProcess(zygote_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopVirtualVars) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  // Launch zygote.
  absl::StatusOr<std::pair<std::string, int>> zygote_status =
      client.LaunchZygote(
          "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote_status.ok());

  std::string zygote_process_id = zygote_status->first;
  printf("zygote process id: %s\n", zygote_process_id.c_str());
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  // Set a global environment variable.
  absl::Status var_status = client.SetGlobalVariable("FOOBAR", "BARFOO", true);
  ASSERT_TRUE(var_status.ok());

  absl::StatusOr<std::pair<std::string, int>> virt_status =
      client.LaunchVirtualProcess(
          "modtest", "zygote",
          "${runfiles_dir}/_main/"
          "testdata/module.so",
          "Main",
          {
              .vars =
                  {
                      {.name = "DAVE", .value = "ALLISON", .exported = true},
                  },
              .notify = true,
          });
  ASSERT_TRUE(virt_status.ok());
  std::string virt_process_id = virt_status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  absl::Status s = client.StopProcess(virt_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);

  // Stop zygote
  s = client.StopProcess(zygote_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndKillVirtual) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2");

  // Launch zygote.
  absl::StatusOr<std::pair<std::string, int>> zygote_status =
      client.LaunchZygote(
          "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote_status.ok());

  std::string zygote_process_id = zygote_status->first;
  printf("zygote process id: %s\n", zygote_process_id.c_str());
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  absl::StatusOr<std::pair<std::string, int>> virt_status =
      client.LaunchVirtualProcess("modtest", "zygote",
                                  "${runfiles_dir}/_main/"
                                  "testdata/module.so",
                                  "Main", {.notify = true});
  ASSERT_TRUE(virt_status.ok());
  int pid = virt_status->second;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  kill(pid, SIGTERM);
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);

  // Stop zygote
  absl::Status s = client.StopProcess(zygote_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, VirtualOutput) {
  adastra::stagezero::Client client;
  InitClient(client, "foobar2", adastra::kOutputEvents);

  // Launch zygote.
  absl::StatusOr<std::pair<std::string, int>> zygote_status =
      client.LaunchZygote(
          "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote_status.ok());

  std::string zygote_process_id = zygote_status->first;
  printf("zygote process id: %s\n", zygote_process_id.c_str());
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  adastra::Stream output = {
      .stream_fd = 1,
      .disposition = adastra::Stream::Disposition::kClient,
      .direction = adastra::Stream::Direction::kOutput,
  };

  absl::StatusOr<std::pair<std::string, int>> virt_status =
      client.LaunchVirtualProcess("modtest", "zygote",
                                  "${runfiles_dir}/_main/"
                                  "testdata/module.so",
                                  "Main", {.streams = {output}});
  ASSERT_TRUE(virt_status.ok());
  std::string virt_process_id = virt_status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);
  std::string data = WaitForOutput(client, "loop 2");
  std::cout << data;

  std::cerr << "Stopping virtual proc\n";

  absl::Status s = client.StopProcess(virt_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);

  // Stop zygote
  std::cerr << "Stopping zygote\n";
  s = client.StopProcess(zygote_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

// Returns the next event that isn't output.
static std::shared_ptr<adastra::stagezero::control::Event>
NextNonOutputEvent(adastra::stagezero::Client &client) {
  for (;;) {
    absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
        client.WaitForEvent();
    if (!e.ok()) {
      ADD_FAILURE() << e.status();
      return nullptr;
    }
    if ((*e)->event_case() != adastra::stagezero::control::Event::kOutput) {
      return *e;
    }
  }
}

TEST_F(ClientTest, ConcurrentVirtualLaunchesFromOneZygote) {
  adastra::stagezero::Client owner;
  InitClient(owner, "concurrent_zygote_owner");

  absl::StatusOr<std::pair<std::string, int>> zygote = owner.LaunchZygote(
      "concurrent_zygote",
      "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote.ok()) << zygote.status();
  WaitForEvent(owner, adastra::stagezero::control::Event::kStart);

  // StageZero serves each client in its own coroutine, so launches from
  // separate clients put several spawn requests in flight to the zygote at
  // once.
  constexpr int kNumLaunches = 8;
  std::vector<std::unique_ptr<adastra::stagezero::Client>> clients;
  for (int i = 0; i < kNumLaunches; i++) {
    clients.push_back(std::make_unique<adastra::stagezero::Client>());
    InitClient(*clients.back(), absl::StrFormat("concurrent_launcher_%d", i),
               adastra::kOutputEvents);
  }

  adastra::Stream output = {
      .stream_fd = 1,
      .disposition = adastra::Stream::Disposition::kClient,
      .direction = adastra::Stream::Direction::kOutput,
  };
  std::vector<absl::StatusOr<std::pair<std::string, int>>> launches(
      kNumLaunches, absl::UnknownError("not launched"));
  std::vector<std::thread> threads;
  for (int i = 0; i < kNumLaunches; i++) {
    threads.emplace_back([&clients, &launches, &output, i]() {
      launches[i] = clients[i]->LaunchVirtualProcess(
          absl::StrFormat("vp_%d", i), "concurrent_zygote",
          "${runfiles_dir}/_main/testdata/module.so", "Main",
          {.streams = {output}, .notify = true});
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  for (int i = 0; i < kNumLaunches; i++) {
    ASSERT_TRUE(launches[i].ok()) << launches[i].status();
    // The process prints its real pid.  A spawn response matched to the wrong
    // request would have given this launch another process's pid.
    std::string expected = absl::StrFormat("pid: %d\n", launches[i]->second);
    bool got_start = false;
    std::string data;
    while (!got_start || data.find("pid: ") == std::string::npos ||
           data.find('\n', data.find("pid: ")) == std::string::npos) {
      absl::StatusOr<std::shared_ptr<adastra::stagezero::control::Event>> e =
          clients[i]->WaitForEvent();
      ASSERT_TRUE(e.ok()) << e.status();
      if ((*e)->event_case() == adastra::stagezero::control::Event::kOutput) {
        data += (*e)->output().data();
      } else {
        ASSERT_EQ(adastra::stagezero::control::Event::kStart,
                  (*e)->event_case());
        got_start = true;
      }
    }
    EXPECT_NE(data.find(expected), std::string::npos)
        << "launch " << i << " expected " << expected << "got: " << data;
  }

  for (int i = 0; i < kNumLaunches; i++) {
    ASSERT_TRUE(clients[i]->StopProcess(launches[i]->first).ok());
    std::shared_ptr<adastra::stagezero::control::Event> event =
        NextNonOutputEvent(*clients[i]);
    ASSERT_NE(nullptr, event);
    ASSERT_EQ(adastra::stagezero::control::Event::kStop, event->event_case());
  }
  ASSERT_TRUE(owner.StopProcess(zygote->first).ok());
  WaitForEvent(owner, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, VirtualLaunchDoesNotWaitForReadiness) {
  adastra::stagezero::Client client;
  InitClient(client, "virtual_launch_no_wait");

  absl::StatusOr<std::pair<std::string, int>> zygote = client.LaunchZygote(
      "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote.ok()) << zygote.status();
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  // Neither process ever notifies, so with a long startup timeout a launch
  // that waited for readiness would not return within the test's lifetime.
  std::vector<std::string> process_ids;
  for (int i = 0; i < 2; i++) {
    absl::StatusOr<std::pair<std::string, int>> virt =
        client.LaunchVirtualProcess(
            absl::StrFormat("never_ready_%d", i), "zygote",
            "${runfiles_dir}/_main/testdata/module.so", "Main",
            {.args = {"no_notify"}, .startup_timeout_secs = 600, .notify = true});
    ASSERT_TRUE(virt.ok()) << virt.status();
    process_ids.push_back(virt->first);
  }

  for (auto &process_id : process_ids) {
    ASSERT_TRUE(client.StopProcess(process_id).ok());
    std::shared_ptr<adastra::stagezero::control::Event> event =
        NextNonOutputEvent(client);
    ASSERT_NE(nullptr, event);
    ASSERT_EQ(adastra::stagezero::control::Event::kStop, event->event_case());
    EXPECT_EQ(process_id, event->stop().process_id());
  }
  ASSERT_TRUE(client.StopProcess(zygote->first).ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, VirtualProcessThatExitsBeforeNotifyingIsReportedStopped) {
  adastra::stagezero::Client client;
  InitClient(client, "virtual_exit_before_notify");

  absl::StatusOr<std::pair<std::string, int>> zygote = client.LaunchZygote(
      "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote.ok()) << zygote.status();
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  // As for a static process, the launch succeeds once the process exists and
  // its exit before becoming ready is reported as a stop with no start.
  absl::StatusOr<std::pair<std::string, int>> virt =
      client.LaunchVirtualProcess(
          "modtest", "zygote", "${runfiles_dir}/_main/testdata/module.so",
          "Main", {.args = {"exit_before_notify"}, .notify = true});
  ASSERT_TRUE(virt.ok()) << virt.status();
  std::shared_ptr<adastra::stagezero::control::Event> event =
      NextNonOutputEvent(client);
  ASSERT_NE(nullptr, event);
  ASSERT_EQ(adastra::stagezero::control::Event::kStop, event->event_case());
  EXPECT_EQ(virt->first, event->stop().process_id());
  EXPECT_EQ(adastra::stagezero::control::StopEvent::EXIT,
            event->stop().reason());
  EXPECT_EQ(3, event->stop().sig_or_status());

  ASSERT_TRUE(client.StopProcess(zygote->first).ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, VirtualProcessThatNeverNotifiesIsStoppedAtStartupTimeout) {
  adastra::stagezero::Client client;
  InitClient(client, "virtual_never_notifies");

  absl::StatusOr<std::pair<std::string, int>> zygote = client.LaunchZygote(
      "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote.ok()) << zygote.status();
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  absl::StatusOr<std::pair<std::string, int>> virt =
      client.LaunchVirtualProcess(
          "modtest", "zygote", "${runfiles_dir}/_main/testdata/module.so",
          "Main",
          {.args = {"no_notify"}, .startup_timeout_secs = 1, .notify = true});
  ASSERT_TRUE(virt.ok()) << virt.status();
  std::shared_ptr<adastra::stagezero::control::Event> event =
      NextNonOutputEvent(client);
  ASSERT_NE(nullptr, event);
  ASSERT_EQ(adastra::stagezero::control::Event::kStop, event->event_case());
  EXPECT_EQ(virt->first, event->stop().process_id());

  ASSERT_TRUE(client.StopProcess(zygote->first).ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, ZygoteWithVirtualProcessGroupButNoUserIsRejected) {
  adastra::stagezero::Client client;
  InitClient(client, "zygote_group_without_user");

  absl::StatusOr<std::pair<std::string, int>> zygote = client.LaunchZygote(
      "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote",
      {.virtual_process_group = "nogroup"});
  ASSERT_FALSE(zygote.ok());
  EXPECT_NE(zygote.status().ToString().find(
                "virtual_process_group without virtual_process_user"),
            std::string::npos)
      << zygote.status();
}

TEST_F(ClientTest, VirtualProcessesDefaultToZygoteVirtualProcessUser) {
  if (geteuid() != 0) {
    GTEST_SKIP() << "Changing process credentials requires root";
  }
  struct passwd *nobody = getpwnam("nobody");
  struct group *adm = getgrnam("adm");
  if (nobody == nullptr || adm == nullptr) {
    GTEST_SKIP() << "Needs user nobody and group adm";
  }

  adastra::stagezero::Client client;
  InitClient(client, "zygote_virtual_process_user", adastra::kOutputEvents);

  // The zygote itself runs as root, so it can switch each virtual process to
  // its own user.
  absl::StatusOr<std::pair<std::string, int>> zygote = client.LaunchZygote(
      "zygote", "${runfiles_dir}/_main/stagezero/zygote/standard_zygote",
      {.virtual_process_user = "nobody"});
  ASSERT_TRUE(zygote.ok()) << zygote.status();
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  adastra::Stream output = {
      .stream_fd = 1,
      .disposition = adastra::Stream::Disposition::kClient,
      .direction = adastra::Stream::Direction::kOutput,
  };
  auto run_probe = [&](const std::string &user, const std::string &group,
                       std::vector<std::string> capabilities = {}) {
    absl::StatusOr<std::pair<std::string, int>> virt =
        client.LaunchVirtualProcess(
            "probe", "zygote",
            "${runfiles_dir}/_main/testdata/process_state_module.so", "Main",
            {.streams = {output},
             .user = user,
             .group = group,
             .capabilities = {.capabilities = std::move(capabilities)}});
    EXPECT_TRUE(virt.ok()) << virt.status();
    bool got_stop = false;
    std::string data = WaitForOutput(client, "done\n", &got_stop, 100);
    if (!got_stop) {
      std::shared_ptr<adastra::stagezero::control::Event> event =
          NextNonOutputEvent(client);
      while (event != nullptr &&
             event->event_case() != adastra::stagezero::control::Event::kStop) {
        event = NextNonOutputEvent(client);
      }
    }
    return data;
  };

  // No user or group: the zygote's virtual process user and that user's
  // primary group.  Credentials changed without an exec must not leave the
  // process undumpable.
  std::string data = run_probe("", "");
  EXPECT_NE(data.find(absl::StrFormat("uid: %d gid: %d dumpable: 1",
                                      nobody->pw_uid, nobody->pw_gid)),
            std::string::npos)
      << data;
  EXPECT_NE(data.find("cap_eff: 0\n"), std::string::npos) << data;

  // Its own group but no user: still the zygote's virtual process user.
  data = run_probe("", "adm");
  EXPECT_NE(data.find(absl::StrFormat("uid: %d gid: %d dumpable: 1",
                                      nobody->pw_uid, adm->gr_gid)),
            std::string::npos)
      << data;

  // Its own user and group.  The group is set before dropping root.
  data = run_probe("nobody", "adm");
  EXPECT_NE(data.find(absl::StrFormat("uid: %d gid: %d dumpable: 1",
                                      nobody->pw_uid, adm->gr_gid)),
            std::string::npos)
      << data;

  // Requested capabilities survive the change of user, and only those.
  data = run_probe("nobody", "", {"CAP_NET_BIND_SERVICE"});
  EXPECT_NE(data.find(absl::StrFormat("cap_eff: %llx\n",
                                      1ULL << CAP_NET_BIND_SERVICE)),
            std::string::npos)
      << data;

  ASSERT_TRUE(client.StopProcess(zygote->first).ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

TEST_F(ClientTest, Namespaces) {
  if (getuid() != 0) {
    std::cerr << "Skipping Namespace test because we aren't root\n";
    std::cerr << "Rerun using sudo if you want to run this\n";
    return;
  }
  adastra::stagezero::Client client;
  InitClient(client, "foobar2", adastra::kOutputEvents);

  adastra::Stream output = {
      .stream_fd = 1,
      .disposition = adastra::Stream::Disposition::kClient,
      .direction = adastra::Stream::Direction::kOutput,
  };

  std::string cmd;
#if defined(__linux__)
  cmd = "/usr/sbin/ip addr; echo FOO";
#else
  cmd = "/sbin/ifconfig -a; echo FOO";
#endif

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess(
          "ifconfig", "/bin/bash",
          {.args =
               {
                   "-c", cmd,
               },
           .streams =
               {
                   output,
               },
           .ns = adastra::Namespace{
               .type = adastra::stagezero::config::Namespace::NS_NEWNET,
           }});
  std::cerr << status.status() << std::endl;
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, adastra::stagezero::control::Event::kStart);

  std::string data = WaitForOutput(client, "FOO", nullptr, 100);
  std::cout << data;

#if defined(__linux__)
  // On linux, as root, we should only see the loopback interface.
  // This will appear as:
  // 1: lo: <LOOPBACK> mtu 65536 qdisc noop state DOWN group default qlen 1000
  //  link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00
  // FOO
  std::vector<std::string> lines = absl::StrSplit(data, '\n');
  bool lo_found = false;
  bool something_else_found = false;
  for (auto &line : lines) {
    if (line.find("FOO") != std::string::npos) {
      continue;
    }
    if (line.find("lo:") != std::string::npos && line[0] == '1') {
      lo_found = true;
    } else {
      if (line.empty() || isspace(line[0])) {
        continue;
      }
      something_else_found = true;
      break;
    }
  }
  ASSERT_TRUE(lo_found);
  ASSERT_FALSE(something_else_found);
#endif

  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, adastra::stagezero::control::Event::kStop);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  absl::ParseCommandLine(argc, argv);

  return RUN_ALL_TESTS();
}
