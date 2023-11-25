// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "stagezero/client/client.h"
#include "stagezero/stagezero.h"
#include <gtest/gtest.h>

#include <fstream>
#include <inttypes.h>
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
    server_ = std::make_unique<stagezero::StageZero>(scheduler_, addr_,
                                                     server_pipe_[1]);

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
  }

  static void TearDownTestSuite() {
    if (!absl::GetFlag(FLAGS_start_server)) {
      return;
    }
    printf("Stopping StageZero server\n");
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

  void InitClient(stagezero::Client &client, const std::string &name) {
    absl::Status s = client.Init(Addr(), name);
    std::cout << "Init status: " << s << std::endl;
    ASSERT_TRUE(s.ok());
  }

  void WaitForEvent(stagezero::Client &client,
                    stagezero::control::Event::EventCase type) {
    std::cout << "waiting for event " << type << std::endl;
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<stagezero::control::Event>> e =
          client.WaitForEvent();
      std::cout << e.status().ToString() << "\n";
      ASSERT_TRUE(e.ok());
      std::shared_ptr<stagezero::control::Event> event = *e;
      if (event->event_case() == stagezero::control::Event::kOutput) {
        // Ignore output events.
        continue;
      }
      ASSERT_EQ(type, event->event_case());
      std::cout << event->DebugString();
      return;
    }
    FAIL();
  }

  std::string WaitForOutput(stagezero::Client &client, std::string match) {
    std::cout << "waiting for output " << match << "\n";
    std::stringstream s;
    for (int retry = 0; retry < 10; retry++) {
      absl::StatusOr<std::shared_ptr<stagezero::control::Event>> e =
          client.WaitForEvent();
      std::cout << e.status().ToString() << "\n";
      if (!e.ok()) {
        return s.str();
      }
      std::shared_ptr<stagezero::control::Event> event = *e;
      std::cout << event->DebugString();
      if (event->event_case() == stagezero::control::Event::kOutput) {
        s << event->output().data();
        if (s.str().find(match) != std::string::npos) {
          return s.str();
        }
      }
    }
    abort();
  }

  void SendInput(stagezero::Client &client, std::string process_id, int fd,
                 std::string s) {
    absl::Status status = client.SendInput(process_id, fd, s);
    ASSERT_TRUE(status.ok());
  }

  static const toolbelt::InetAddress &Addr() { return addr_; }

  co::CoroutineScheduler &Scheduler() const { return scheduler_; }

  static stagezero::StageZero *Server() { return server_.get(); }

private:
  static co::CoroutineScheduler scheduler_;
  static std::string socket_;
  static int server_pipe_[2];
  static std::unique_ptr<stagezero::StageZero> server_;
  static std::thread server_thread_;
  static toolbelt::InetAddress addr_;
};

co::CoroutineScheduler ClientTest::scheduler_;
int ClientTest::server_pipe_[2];
std::unique_ptr<stagezero::StageZero> ClientTest::server_;
std::thread ClientTest::server_thread_;
toolbelt::InetAddress ClientTest::addr_;

void SignalHandler(int sig) {
  printf("Signal %d\n", sig);
  ClientTest::Server()->Stop();
  ClientTest::WaitForServerStop();

  signal(sig, SIG_DFL);
  raise(sig);
}

TEST_F(ClientTest, Init) {
  stagezero::Client client;
  InitClient(client, "foobar1");
}

TEST_F(ClientTest, LaunchAndStop) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop",
                                 "${runfiles_dir}/__main__/testdata/loop",
                                 {
                                     .args =
                                         {
                                             "ignore_signal",
                                         },
                                     .notify = true,
                                 });
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopSigTerm) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop",
                                 "${runfiles_dir}/__main__/testdata/loop",
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
  WaitForEvent(client, stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopSigKill) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop",
                                 "${runfiles_dir}/__main__/testdata/loop",
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
  WaitForEvent(client, stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndKill) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop",
                                 "${runfiles_dir}/__main__/testdata/loop",
                                 {
                                     .notify = true,
                                 });
  ASSERT_TRUE(status.ok());
  int pid = status->second;
  WaitForEvent(client, stagezero::control::Event::kStart);
  std::cout << "killing process " << pid << std::endl;
  kill(pid, SIGTERM);
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopNoNotify) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop",
                                 "${runfiles_dir}/__main__/testdata/loop",
                                 {.args = {
                                      "ignore_signal",
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, stagezero::control::Event::kStart);
  sleep(1);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopRepeated) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  for (int i = 0; i < 10; i++) {
    absl::StatusOr<std::pair<std::string, int>> status =
        client.LaunchStaticProcess(
            "loop", "${runfiles_dir}/__main__/testdata/loop",
            {
                .notify = true, .startup_timeout_secs = 1,
            });
    ASSERT_TRUE(status.ok());
    std::string process_id = status->first;
    WaitForEvent(client, stagezero::control::Event::kStart);
    std::cout << "stopping process " << process_id << std::endl;
    absl::Status s = client.StopProcess(process_id);
    ASSERT_TRUE(s.ok());
    WaitForEvent(client, stagezero::control::Event::kStop);
  }
}

TEST_F(ClientTest, LaunchAndStopRepeatedNewClient) {
  for (int i = 0; i < 10; i++) {
    stagezero::Client client;
    InitClient(client, "foobar2");

    absl::StatusOr<std::pair<std::string, int>> status =
        client.LaunchStaticProcess("loop",
                                   "${runfiles_dir}/__main__/testdata/loop",
                                   {
                                       .notify = true,
                                   });
    ASSERT_TRUE(status.ok());
    std::string process_id = status->first;
    WaitForEvent(client, stagezero::control::Event::kStart);
    std::cout << "stopping process " << process_id << std::endl;
    absl::Status s = client.StopProcess(process_id);
    ASSERT_TRUE(s.ok());
    WaitForEvent(client, stagezero::control::Event::kStop);
  }
}

TEST_F(ClientTest, Output) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  stagezero::Stream output = {
      .stream_fd = 1,
      .disposition = stagezero::Stream::Disposition::kClient,
      .direction = stagezero::Stream::Direction::kOutput,
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop",
                                 "${runfiles_dir}/__main__/testdata/loop",
                                 {.streams = {
                                      output,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, stagezero::control::Event::kStart);
  std::string data = WaitForOutput(client, "loop 2");
  std::cout << data;

  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
  // sleep(2);
  // std::cout << "sleep done" << std::endl;
}

TEST_F(ClientTest, OutputTTY) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  stagezero::Stream output = {
      .stream_fd = 1,
      .tty = true,
      .disposition = stagezero::Stream::Disposition::kClient,
      .direction = stagezero::Stream::Direction::kOutput,
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("loop2",
                                 "${runfiles_dir}/__main__/testdata/loop",
                                 {.streams = {
                                      output,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;
  WaitForEvent(client, stagezero::control::Event::kStart);
  std::string data = WaitForOutput(client, "loop 2");
  std::cout << data;

  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, Echo) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  stagezero::Stream output = {
      .stream_fd = 1,
      .disposition = stagezero::Stream::Disposition::kClient,
      .direction = stagezero::Stream::Direction::kOutput,
  };

  stagezero::Stream input = {
      .stream_fd = 0,
      .disposition = stagezero::Stream::Disposition::kClient,
      .direction = stagezero::Stream::Direction::kInput,
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("echo",
                                 "${runfiles_dir}/__main__/testdata/echo",
                                 {.streams = {
                                      output, input,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;

  // The echo program prints "running" when it starts.
  WaitForEvent(client, stagezero::control::Event::kStart);
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
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, EchoFileRead) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  stagezero::Stream output = {
      .stream_fd = 1,
      .disposition = stagezero::Stream::Disposition::kClient,
      .direction = stagezero::Stream::Direction::kOutput,
  };

  stagezero::Stream input = {
      .stream_fd = 0,
      .disposition = stagezero::Stream::Disposition::kFile,
      .direction = stagezero::Stream::Direction::kInput,
      .data =
          {
              "${runfiles_dir}/__main__/testdata/input_data.txt",
          },
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("echo",
                                 "${runfiles_dir}/__main__/testdata/echo",
                                 {.streams = {
                                      output, input,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;

  WaitForEvent(client, stagezero::control::Event::kStart);

  std::string data = WaitForOutput(client, "from a file\ndone\n");
  std::cout << "output: " << data;

  // Stop the process.
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, EchoFileWrite) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  stagezero::Stream output = {
      .stream_fd = 1,
      .disposition = stagezero::Stream::Disposition::kFile,
      .direction = stagezero::Stream::Direction::kOutput,
      .data =
          {
              "/tmp/echo.txt",
          },
  };

  stagezero::Stream input = {
      .stream_fd = 0,
      .disposition = stagezero::Stream::Disposition::kFile,
      .direction = stagezero::Stream::Direction::kInput,
      .data =
          {
              "${runfiles_dir}/__main__/testdata/input_data.txt",
          },
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess("echo",
                                 "${runfiles_dir}/__main__/testdata/echo",
                                 {.streams = {
                                      output, input,
                                  }});
  ASSERT_TRUE(status.ok());
  std::string process_id = status->first;

  WaitForEvent(client, stagezero::control::Event::kStart);

  // Process will exit itself.
  WaitForEvent(client, stagezero::control::Event::kStop);

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
  stagezero::Client client;
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
  stagezero::Client client;
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

  var_status2 = client.GetGlobalVariable("runfiles_dir");
  ASSERT_TRUE(var_status2.ok());
  std::string runfiles = std::move(var_status2->first);

  // Start the 'vars' process to print args and env.

  stagezero::Stream output = {
      .stream_fd = 1,
      .disposition = stagezero::Stream::Disposition::kClient,
      .direction = stagezero::Stream::Direction::kOutput,
  };

  absl::StatusOr<std::pair<std::string, int>> status =
      client.LaunchStaticProcess(
          "vars", "${runfiles_dir}/__main__/testdata/${procname}",
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
  WaitForEvent(client, stagezero::control::Event::kStart);

  std::string data = WaitForOutput(client, "DONE\n");

  std::string expected = "arg[0]: " + runfiles +
                         R"(/__main__/testdata/vars
arg[1]: allison123
arg[2]: barfoo 456
DAVE=ALLISON
FOOBAR=BARFOO
DONE
)";
  ASSERT_EQ(expected, data);
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchZygoteAndStop) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status = client.LaunchZygote(
      "zygote", "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(status.ok());

  std::string process_id = status->first;
  printf("process id: %s\n", process_id.c_str());
  WaitForEvent(client, stagezero::control::Event::kStart);
  std::cout << "stopping process " << process_id << std::endl;
  absl::Status s = client.StopProcess(process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchZygoteAndKill) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  absl::StatusOr<std::pair<std::string, int>> status = client.LaunchZygote(
      "zygote", "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(status.ok());

  int pid = status->second;
  WaitForEvent(client, stagezero::control::Event::kStart);
  std::cout << "killing process " << pid << std::endl;
  kill(pid, SIGTERM);
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, Launch2ZygotesAndStop) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  std::vector<std::string> process_ids;
  for (int i = 0; i < 2; i++) {
    absl::StatusOr<std::pair<std::string, int>> status = client.LaunchZygote(
        absl::StrFormat("zygote_%d", i),
        "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote");
    ASSERT_TRUE(status.ok());

    std::string process_id = status->first;
    process_ids.push_back(process_id);
    WaitForEvent(client, stagezero::control::Event::kStart);
  }
  for (auto &process_id : process_ids) {
    std::cout << "stopping process " << process_id << std::endl;
    absl::Status s = client.StopProcess(process_id);
    ASSERT_TRUE(s.ok());
    WaitForEvent(client, stagezero::control::Event::kStop);
  }
}

TEST_F(ClientTest, LaunchAndStopVirtual) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  // Launch zygote.
  absl::StatusOr<std::pair<std::string, int>> zygote_status =
      client.LaunchZygote(
          "zygote",
          "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote_status.ok());

  std::string zygote_process_id = zygote_status->first;
  printf("zygote process id: %s\n", zygote_process_id.c_str());
  WaitForEvent(client, stagezero::control::Event::kStart);

  absl::StatusOr<std::pair<std::string, int>> virt_status =
      client.LaunchVirtualProcess("modtest", "zygote",
                                  "${runfiles_dir}/__main__/"
                                  "testdata/module.so",
                                  "Main", {.args = {"ignore_signal"}});
  ASSERT_TRUE(virt_status.ok());
  std::string virt_process_id = virt_status->first;
  WaitForEvent(client, stagezero::control::Event::kStart);

  absl::Status s = client.StopProcess(virt_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);

  // Stop zygote
  s = client.StopProcess(zygote_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndStopVirtualVars) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  // Launch zygote.
  absl::StatusOr<std::pair<std::string, int>> zygote_status =
      client.LaunchZygote(
          "zygote",
          "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote_status.ok());

  std::string zygote_process_id = zygote_status->first;
  printf("zygote process id: %s\n", zygote_process_id.c_str());
  WaitForEvent(client, stagezero::control::Event::kStart);

  // Set a global environment variable.
  absl::Status var_status = client.SetGlobalVariable("FOOBAR", "BARFOO", true);
  ASSERT_TRUE(var_status.ok());

  absl::StatusOr<std::pair<std::string, int>> virt_status =
      client.LaunchVirtualProcess(
          "modtest", "zygote",
          "${runfiles_dir}/__main__/"
          "testdata/module.so",
          "Main",
          {
              .vars =
                  {
                      {.name = "DAVE", .value = "ALLISON", .exported = true},
                  },
          });
  ASSERT_TRUE(virt_status.ok());
  std::string virt_process_id = virt_status->first;
  WaitForEvent(client, stagezero::control::Event::kStart);

  absl::Status s = client.StopProcess(virt_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);

  // Stop zygote
  s = client.StopProcess(zygote_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, LaunchAndKillVirtual) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  // Launch zygote.
  absl::StatusOr<std::pair<std::string, int>> zygote_status =
      client.LaunchZygote(
          "zygote",
          "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote_status.ok());

  std::string zygote_process_id = zygote_status->first;
  printf("zygote process id: %s\n", zygote_process_id.c_str());
  WaitForEvent(client, stagezero::control::Event::kStart);

  absl::StatusOr<std::pair<std::string, int>> virt_status =
      client.LaunchVirtualProcess("modtest", "zygote",
                                  "${runfiles_dir}/__main__/"
                                  "testdata/module.so",
                                  "Main");
  ASSERT_TRUE(virt_status.ok());
  int pid = virt_status->second;
  WaitForEvent(client, stagezero::control::Event::kStart);

  kill(pid, SIGTERM);
  WaitForEvent(client, stagezero::control::Event::kStop);

  // Stop zygote
  absl::Status s = client.StopProcess(zygote_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

TEST_F(ClientTest, VirtualOutput) {
  stagezero::Client client;
  InitClient(client, "foobar2");

  // Launch zygote.
  absl::StatusOr<std::pair<std::string, int>> zygote_status =
      client.LaunchZygote(
          "zygote",
          "${runfiles_dir}/__main__/stagezero/zygote/standard_zygote");
  ASSERT_TRUE(zygote_status.ok());

  std::string zygote_process_id = zygote_status->first;
  printf("zygote process id: %s\n", zygote_process_id.c_str());
  WaitForEvent(client, stagezero::control::Event::kStart);

  stagezero::Stream output = {
      .stream_fd = 1,
      .disposition = stagezero::Stream::Disposition::kClient,
      .direction = stagezero::Stream::Direction::kOutput,
  };

  absl::StatusOr<std::pair<std::string, int>> virt_status =
      client.LaunchVirtualProcess("modtest", "zygote",
                                  "${runfiles_dir}/__main__/"
                                  "testdata/module.so",
                                  "Main", {.streams = {output}});
  ASSERT_TRUE(virt_status.ok());
  std::string virt_process_id = virt_status->first;
  WaitForEvent(client, stagezero::control::Event::kStart);
  std::string data = WaitForOutput(client, "loop 2");
  std::cout << data;

  std::cerr << "Stopping virtual proc\n";

  absl::Status s = client.StopProcess(virt_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);

  // Stop zygote
  std::cerr << "Stopping zygote\n";
  s = client.StopProcess(zygote_process_id);
  ASSERT_TRUE(s.ok());
  WaitForEvent(client, stagezero::control::Event::kStop);
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  absl::ParseCommandLine(argc, argv);

  return RUN_ALL_TESTS();
}