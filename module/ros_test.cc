// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "client/client.h"
#include "module/ros_module.h"
#include "module/serdes/testdata/Test.h"
#include "module/zeros/testdata/Test.h"
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
template <typename T> using ROSSubscriber = adastra::module::ROSSubscriber<T>;
template <typename T> using ROSPublisher = adastra::module::ROSPublisher<T>;
template <typename T>
using ZerosSubscriber = adastra::module::ZerosSubscriber<T>;
template <typename T> using ZerosPublisher = adastra::module::ZerosPublisher<T>;
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
  MyModule() : Module(ModuleTest::Symbols()) {}

  absl::Status Init(int argc, char **argv) override { return absl::OkStatus(); }
};

// Publish and subscribe to a ROS message in serialized ROS format.
TEST_F(ModuleTest, PubSub) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  using M = testdata::Test;

  auto p = mod.RegisterPublisher<M>(
      "foobar", 256, 10,
      [](std::shared_ptr<ROSPublisher<M>> pub, M &msg,
         co::Coroutine *c) -> size_t {
        msg.x = 1234;
        msg.s = "dave";
        return msg.SerializedSize();
      });
  ASSERT_TRUE(p.ok());
  auto pub = *p;

  auto sub = mod.RegisterSubscriber<M>(
      "foobar", [&mod](std::shared_ptr<ROSSubscriber<M>> sub,
                       Message<const M> msg, co::Coroutine *c) {
        ASSERT_EQ(1234, msg->x);
        ASSERT_EQ("dave", msg->s);
        mod.Stop();
      });
  ASSERT_TRUE(sub.ok());

  pub->Publish();
  mod.Run();
}

TEST_F(ModuleTest, PubSubZeroCopy) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  using M = testdata::zeros::Test;

  auto p = mod.RegisterPublisher<M>(
      "foobar", 256, 10,
      [](std::shared_ptr<ZerosPublisher<M>> pub,
         M &msg, co::Coroutine *c) -> size_t {
        msg.x = 1234;
        msg.s = "dave";
        return msg.Size();
      });
  ASSERT_TRUE(p.ok());
  auto pub = *p;

  auto sub = mod.RegisterSubscriber<M>(
      "foobar",
      [&mod](std::shared_ptr<ZerosSubscriber<M>> sub,
             Message<const M> msg, co::Coroutine *c) {
        ASSERT_EQ(1234, msg->x);
        std::string_view s = msg->s;
        ASSERT_EQ("dave", s);
        mod.Stop();
      });
  ASSERT_TRUE(sub.ok());

  pub->Publish();
  mod.Run();
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  absl::ParseCommandLine(argc, argv);

  return RUN_ALL_TESTS();
}
