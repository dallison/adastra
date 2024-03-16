// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "client/client.h"
#include "module/ros_module.h"
#include "module/testdata/Test.h"
#include "server/server.h"
#include "toolbelt/hexdump.h"
#include <gtest/gtest.h>

#include <fstream>
#include <inttypes.h>
#include <memory>
#include <signal.h>
#include <sstream>
#include <sys/resource.h>
#include <thread>

using namespace adastra::module::frequency_literals;

void SignalHandler(int sig) { printf("Signal %d", sig); }

template <typename T> using Message = adastra::module::Message<T>;
template <typename T>
using Subscriber = adastra::module::ROSSubscriber<T>;
template <typename T> using Publisher = adastra::module::ROSPublisher<T>;
using ROSModule = adastra::module::ROSModule;
using namespace std::chrono_literals;

class ModuleTest : public ::testing::Test {
public:
  // We run one server for the duration of the whole test suite.
  static void SetUpTestSuite() {
    printf("Starting Subspace server\n");
    char tmp[] = "/tmp/subspaceXXXXXX";
    int fd = mkstemp(tmp);
    ASSERT_NE(-1, fd);
    socket_ = tmp;
    close(fd);

    // The server will write to this pipe to notify us when it
    // has started and stopped.  This end of the pipe is blocking.
    (void)pipe(server_pipe_);

    server_ =
        std::make_unique<subspace::Server>(scheduler_, socket_, "", 0, 0,
                                           /*local=*/true, server_pipe_[1]);

    // Start server running in a thread.
    server_thread_ = std::thread([]() {
      absl::Status s = server_->Run();
      if (!s.ok()) {
        fprintf(stderr, "Error running Subspace server: %s\n",
                s.ToString().c_str());
        exit(1);
      }
    });

    // Wait for server to tell us that it's running.
    char buf[8];
    (void)::read(server_pipe_[0], buf, 8);
  }

  static void TearDownTestSuite() {
    printf("Stopping Subspace server\n");
    server_->Stop();

    // Wait for server to tell us that it's stopped.
    char buf[8];
    (void)::read(server_pipe_[0], buf, 8);
    server_thread_.join();
  }

  void SetUp() override { signal(SIGPIPE, SIG_IGN); }
  void TearDown() override {}

  static const std::string &Socket() { return socket_; }

  static std::unique_ptr<adastra::stagezero::SymbolTable> Symbols() {
    auto symbols = std::make_unique<adastra::stagezero::SymbolTable>();
    symbols->AddSymbol("name", "test", false);
    symbols->AddSymbol("subspace_socket", Socket(), false);
    return symbols;
  }

private:
  static co::CoroutineScheduler scheduler_;
  static std::string socket_;
  static int server_pipe_[2];
  static std::unique_ptr<subspace::Server> server_;
  static std::thread server_thread_;
};

co::CoroutineScheduler ModuleTest::scheduler_;
std::string ModuleTest::socket_ = "/tmp/subspace";
int ModuleTest::server_pipe_[2];
std::unique_ptr<subspace::Server> ModuleTest::server_;
std::thread ModuleTest::server_thread_;

class MyModule : public ROSModule {
public:
  MyModule() : ROSModule(ModuleTest::Symbols()) {}

  absl::Status Init(int argc, char **argv) override { return absl::OkStatus(); }
};

// Publish and subscribe to a ROS message in serialized ROS format.
TEST_F(ModuleTest, PubSub) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  auto p = mod.RegisterPublisher<testdata::Test>(
      "foobar", 256, 10,
      [](std::shared_ptr<Publisher<testdata::Test>> pub,
         testdata::Test &msg, co::Coroutine *c) -> bool {
          msg.x = 1234;
          msg.s = "dave";
        return true;
      });
  ASSERT_TRUE(p.ok());
  auto pub = *p;

  auto sub = mod.RegisterSubscriber<testdata::Test>(
      "foobar", [&mod](std::shared_ptr<Subscriber<testdata::Test>> sub,
                       Message<const testdata::Test> msg,
                       co::Coroutine *c) { 
                        ASSERT_EQ(1234, msg->x);
                        ASSERT_EQ("dave", msg->s);
                        mod.Stop(); });
  ASSERT_TRUE(sub.ok());

  pub->Publish();
  mod.Run();
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  absl::ParseCommandLine(argc, argv);

  return RUN_ALL_TESTS();
}
