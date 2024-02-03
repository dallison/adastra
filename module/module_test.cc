// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "client/client.h"
#include "module/protobuf_module.h"
#include "module/testdata/test.pb.h"
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
using Subscriber = adastra::module::ProtobufSubscriber<T>;
template <typename T> using Publisher = adastra::module::ProtobufPublisher<T>;
using ProtobufModule = adastra::module::ProtobufModule;
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

class MyModule : public ProtobufModule {
public:
  MyModule() : ProtobufModule(ModuleTest::Symbols()) {}

  absl::Status Init(int argc, char **argv) override { return absl::OkStatus(); }
};

TEST_F(ModuleTest, Ticker) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  int count = 0;
  mod.RunPeriodically(20, [&mod, &count](co::Coroutine *c) {
    count++;
    if (count == 3) {
      mod.Stop();
    }
  });
  mod.Run();
}

TEST_F(ModuleTest, Timer) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  mod.RunAfterDelay(100ms, [&mod](co::Coroutine *c) { mod.Stop(); });
  mod.Run();
}

TEST_F(ModuleTest, Event) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  int pipes[2];
  pipe(pipes);

  mod.RunOnEvent(pipes[0], [&mod, &pipes](int fd, co::Coroutine *c) {
    ASSERT_EQ(pipes[0], fd);
    mod.Stop();
  });

  mod.RunNow([&pipes](co::Coroutine *c) { write(pipes[1], "x", 1); });
  mod.Run();
  close(pipes[0]);
  close(pipes[1]);
}

TEST_F(ModuleTest, EventFileDescriptor) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  int pipes[2];
  pipe(pipes);

  toolbelt::FileDescriptor r(pipes[0]);

  mod.RunOnEvent(r, [r, &mod](toolbelt::FileDescriptor fd, co::Coroutine *c) {
    ASSERT_EQ(r, fd);
    mod.Stop();
  });

  mod.RunNow([&pipes](co::Coroutine *c) { write(pipes[1], "x", 1); });
  mod.Run();
}

TEST_F(ModuleTest, EventTimeout) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  int pipes[2];
  pipe(pipes);

  mod.RunOnEventWithTimeout(pipes[0], 100ms, [&mod](int fd, co::Coroutine *c) {
    ASSERT_EQ(-1, fd);
    mod.Stop();
  });

  mod.Run();
  close(pipes[0]);
  close(pipes[1]);
}

TEST_F(ModuleTest, EventFileDescriptorTimeout) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  int pipes[2];
  pipe(pipes);

  toolbelt::FileDescriptor r(pipes[0]);

  mod.RunOnEventWithTimeout(
      r, 100ms, [r, &mod](toolbelt::FileDescriptor fd, co::Coroutine *c) {
        ASSERT_FALSE(fd.Valid());
        mod.Stop();
      });

  mod.Run();
}

TEST_F(ModuleTest, PubSub) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  auto p = mod.RegisterPublisher<moduletest::TestMessage>(
      "foobar", 256, 10,
      [](std::shared_ptr<Publisher<moduletest::TestMessage>> pub,
         moduletest::TestMessage &msg, co::Coroutine *c) -> bool {
        msg.set_x(1234);
        msg.set_s("dave");
        return true;
      });
  ASSERT_TRUE(p.ok());
  auto pub = *p;

  auto sub = mod.RegisterSubscriber<moduletest::TestMessage>(
      "foobar", [&mod](std::shared_ptr<Subscriber<moduletest::TestMessage>> sub,
                       Message<const moduletest::TestMessage> msg,
                       co::Coroutine *c) { mod.Stop(); });
  ASSERT_TRUE(sub.ok());

  pub->Publish();
  mod.Run();
}

TEST_F(ModuleTest, PubSubZeroCopy) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  int count = 0;
  auto sub = mod.RegisterZeroCopySubscriber<std::byte>(
      "foobar",
      [&mod, &count](
          std::shared_ptr<adastra::module::ZeroCopySubscriber<std::byte>> sub,
          Message<const std::byte> msg, co::Coroutine *c) {
        absl::Span<const std::byte> span = msg;
        ASSERT_EQ(6, span.size());
        ASSERT_STREQ("foobar", reinterpret_cast<const char *>(span.data()));
        count++;
        if (count == 5) {
          mod.Stop();
        }
      });
  ASSERT_TRUE(sub.ok());

  auto p =
      mod.RegisterZeroCopyPublisher<absl::Span<std::byte>>("foobar", 256, 10);
  ASSERT_TRUE(p.ok());
  auto pub = std::move(*p);

  mod.RunNow([&pub](co::Coroutine *c) {
    for (int i = 0; i < 5; i++) {
      absl::StatusOr<void *> buffer = pub->GetMessageBuffer(20, c);
      ASSERT_TRUE(buffer.ok());
      ASSERT_NE(nullptr, *buffer);
      int length = snprintf(reinterpret_cast<char *>(*buffer), 256, "foobar");
      pub->Publish(length, c);
      c->Sleep(1);
    }
  });
  mod.Run();
}

TEST_F(ModuleTest, PubSubZeroCopyStruct) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  struct MyMessage {
    int x;
    char s[20];
  };

  auto sub = mod.RegisterZeroCopySubscriber<MyMessage>(
      "foobar",
      [&mod](
          std::shared_ptr<adastra::module::ZeroCopySubscriber<MyMessage>> sub,
          Message<const MyMessage> msg, co::Coroutine *c) {
        ASSERT_EQ(1234, msg->x);
        ASSERT_STREQ("foobar", msg->s);
        mod.Stop();
      });
  ASSERT_TRUE(sub.ok());

  auto p = mod.RegisterZeroCopyPublisher<MyMessage>("foobar", 256, 10);
  ASSERT_TRUE(p.ok());
  auto pub = std::move(*p);

  mod.RunNow([&pub](co::Coroutine *c) {
    absl::StatusOr<void *> buffer = pub->GetMessageBuffer(sizeof(MyMessage), c);
    ASSERT_TRUE(buffer.ok());
    ASSERT_NE(nullptr, *buffer);
    MyMessage *msg = reinterpret_cast<MyMessage *>(*buffer);
    msg->x = 1234;
    strcpy(msg->s, "foobar");
    pub->Publish(sizeof(MyMessage), c);
  });
  mod.Run();
}

TEST_F(ModuleTest, PubSubZeroCopyStructCallback) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  struct MyMessage {
    int x;
    char s[20];
  };

  auto sub = mod.RegisterZeroCopySubscriber<MyMessage>(
      "foobar",
      [&mod](
          std::shared_ptr<adastra::module::ZeroCopySubscriber<MyMessage>> sub,
          Message<const MyMessage> msg, co::Coroutine *c) {
        ASSERT_EQ(1234, msg->x);
        ASSERT_STREQ("foobar", msg->s);
        mod.Stop();
      });
  ASSERT_TRUE(sub.ok());

  auto p = mod.RegisterZeroCopyPublisher<MyMessage>(
      "foobar", 256, 10,
      [](std::shared_ptr<adastra::module::ZeroCopyPublisher<MyMessage>> pub,
         MyMessage &msg, co::Coroutine *c) -> bool {
        msg.x = 1234;
        strcpy(msg.s, "foobar");
        return true;
      });
  ASSERT_TRUE(p.ok());
  auto pub = std::move(*p);

  mod.RunNow([&pub](co::Coroutine *c) { pub->Publish(); });
  mod.Run();
}

TEST_F(ModuleTest, PublishOnTick) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  auto p = mod.RegisterPublisher<moduletest::TestMessage>(
      "foobar", 256, 10,
      [](std::shared_ptr<Publisher<moduletest::TestMessage>> pub,
         moduletest::TestMessage &msg, co::Coroutine *c) -> bool {
        msg.set_x(1234);
        msg.set_s("dave");
        return true;
      });
  ASSERT_TRUE(p.ok());
  auto pub = *p;

  int count = 0;
  auto sub = mod.RegisterSubscriber<moduletest::TestMessage>(
      "foobar",
      [&mod, &count](std::shared_ptr<Subscriber<moduletest::TestMessage>> sub,
                     Message<const moduletest::TestMessage> msg,
                     co::Coroutine *c) {
        count++;
        if (count == 4) {
          mod.Stop();
        }
      });
  ASSERT_TRUE(sub.ok());

  mod.RunPeriodically(10, [&pub](co::Coroutine *c) { pub->Publish(); });

  mod.Run();
}

TEST_F(ModuleTest, MultipleSubscribers) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  int count = 1;
  auto p = mod.RegisterPublisher<moduletest::TestMessage>(
      "foobar", 256, 10,
      [&count](std::shared_ptr<Publisher<moduletest::TestMessage>> pub,
               moduletest::TestMessage &msg, co::Coroutine *c) -> bool {
        msg.set_x(count++);
        msg.set_s("dave");
        return true;
      });
  ASSERT_TRUE(p.ok());
  auto pub = *p;

  constexpr int kNumSubs = 4;
  constexpr int kNumMessages = 10;
  std::vector<std::shared_ptr<Subscriber<moduletest::TestMessage>>> subs;
  std::vector<int> counts(kNumSubs);
  for (int i = 0; i < kNumSubs; i++) {
    auto sub = mod.RegisterSubscriber<moduletest::TestMessage>(
        "foobar",
        [&mod, i, &counts](
            std::shared_ptr<Subscriber<moduletest::TestMessage>> sub,
            Message<const moduletest::TestMessage> msg, co::Coroutine *c) {
          counts[i]++;

          bool all_done = true;
          for (int j = 0; j < kNumSubs; j++) {
            if (counts[j] != kNumMessages - 1) {
              all_done = false;
              break;
            }
          }
          if (all_done) {
            mod.Stop();
          }
        });
    ASSERT_TRUE(sub.ok());
    subs.push_back(*sub);
  }

  for (int i = 0; i < kNumMessages; i++) {
    pub->Publish();
  }
  mod.Run();
}

TEST_F(ModuleTest, MultiplePublishersAndSubscribers) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  constexpr int kNumPubs = 5;
  constexpr int kNumSubs = 4;
  constexpr int kNumMessages = 10;

  std::vector<std::shared_ptr<Publisher<moduletest::TestMessage>>> pubs;
  std::vector<std::shared_ptr<Subscriber<moduletest::TestMessage>>> subs;
  std::vector<int> counts(kNumSubs);
  int count = 1;

  for (int i = 0; i < kNumPubs; i++) {
    std::string pub_name = absl::StrFormat("pub-%d", i);
    auto pub = mod.RegisterPublisher<moduletest::TestMessage>(
        "foobar", 256, 20,
        [&count,
         pub_name](std::shared_ptr<Publisher<moduletest::TestMessage>> pub,
                   moduletest::TestMessage &msg, co::Coroutine *c) -> bool {
          msg.set_x(count++);
          msg.set_s(pub_name);
          return true;
        });
    ASSERT_TRUE(pub.ok());
    pubs.push_back(*pub);
  }

  for (int i = 0; i < kNumSubs; i++) {
    auto sub = mod.RegisterSubscriber<moduletest::TestMessage>(
        "foobar",
        [&mod, i, &counts](
            std::shared_ptr<Subscriber<moduletest::TestMessage>> sub,
            Message<const moduletest::TestMessage> msg, co::Coroutine *c) {
          counts[i]++;
          bool all_done = true;
          for (int j = 0; j < kNumSubs; j++) {
            if (counts[j] < kNumMessages - 1) {
              all_done = false;
              break;
            }
          }
          if (all_done) {
            mod.Stop();
          }
        });
    ASSERT_TRUE(sub.ok());
    subs.push_back(*sub);
  }

  // Publish at 10Hz
  for (int i = 0; i < kNumPubs; i++) {
    mod.RunPeriodically(10_hz,
                        [&pubs, i](co::Coroutine *c) { pubs[i]->Publish(); });
  }

  mod.Run();
}

TEST_F(ModuleTest, ReliablePubSub) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  // We only have 10 slots.  If we send 100 messages too quickly on a reliable
  // channel, they will still get through.
  constexpr int kNumMessages = 100;

  int pub_count = 1;
  auto p = mod.RegisterPublisher<moduletest::TestMessage>(
      "foobar", 256, 10, {.reliable = true},
      [&pub_count](auto pub, auto &msg, auto c) -> bool {
        msg.set_x(pub_count++);
        msg.set_s("dave");
        return true;
      });
  ASSERT_TRUE(p.ok());
  auto pub = *p;

  int sub_count = 0;
  auto sub = mod.RegisterSubscriber<moduletest::TestMessage>(
      "foobar", {.reliable = true},
      [&mod, &sub_count](auto sub, auto msg, auto c) {
        sub_count++;
        if (sub_count == kNumMessages) {
          mod.Stop();
        }
      });
  ASSERT_TRUE(sub.ok());

  for (int i = 0; i < kNumMessages; i++) {
    pub->Publish();
  }
  mod.Run();
}

// In this test we have two channels, "one" and "two".  We publish to "one"
// and the subscriber reads from it and publishes the incoming message to "two"
// where a subscriber reads it.
// Channel "one" has 10 slots and channel "two" has 5.  So when we run out of
// slots in channel "two" we will apply backpressure to channel "one" and it
// will stop sending.
TEST_F(ModuleTest, ReliablePubSubBackpressure) {
  MyModule mod;
  ASSERT_TRUE(mod.ModuleInit().ok());

  // We only have 10 slots.  If we send 100 messages too quickly on a reliable
  // channel, they will still get through.
  constexpr int kNumMessages = 100;

  std::shared_ptr<Publisher<moduletest::TestMessage>> pub2;

  // Subscriber to "one" and publish to "two"
  auto sub1 = mod.RegisterSubscriber<moduletest::TestMessage>(
      "one", {.reliable = true},
      [&pub2](std::shared_ptr<Subscriber<moduletest::TestMessage>> sub,
              Message<const moduletest::TestMessage> msg,
              co::Coroutine *c) { pub2->Publish(*msg, c); });
  ASSERT_TRUE(sub1.ok());

  // Publisher for "two" that backpressures channel "one"
  auto p2 = mod.RegisterPublisher<moduletest::TestMessage>(
      "two", 256, 5, {.reliable = true, .backpressured_subscribers = {*sub1}});
  ASSERT_TRUE(p2.ok());

  pub2 = *p2;

  // Publisher to "one".
  int pub_count = 1;
  auto p1 = mod.RegisterPublisher<moduletest::TestMessage>(
      "one", 256, 10, {.reliable = true},
      [&pub_count](std::shared_ptr<Publisher<moduletest::TestMessage>> pub,
                   moduletest::TestMessage &msg, co::Coroutine *c) -> bool {
        msg.set_x(pub_count++);
        msg.set_s("dave");
        return true;
      });
  ASSERT_TRUE(p1.ok());
  auto pub1 = *p1;

  // Subscriber to "two", final subscriber in chain.
  int sub_count = 0;
  auto sub2 = mod.RegisterSubscriber<moduletest::TestMessage>(
      "two", {.reliable = true},
      [&mod, &sub_count](
          std::shared_ptr<Subscriber<moduletest::TestMessage>> sub,
          Message<const moduletest::TestMessage> msg, co::Coroutine *c) {
        sub_count++;
        if (sub_count == kNumMessages) {
          mod.Stop();
        }
      });
  ASSERT_TRUE(sub2.ok());

  for (int i = 0; i < kNumMessages; i++) {
    pub1->Publish();
  }
  mod.Run();
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  absl::ParseCommandLine(argc, argv);

  return RUN_ALL_TESTS();
}
