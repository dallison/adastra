#pragma once
#include "absl/types/span.h"
#include "client/client.h"
#include "coroutine.h"
#include "google/protobuf/message.h"
#include "module/subscriber.h"
#include "toolbelt/triggerfd.h"
#include <vector>

namespace adastra::module {

class Module;

struct PublisherOptions {
  bool local = false;
  bool reliable = false;
  bool fixed_size = false;
  std::string type;
  std::vector<std::shared_ptr<SubscriberBase>> backpressured_subscribers;
  size_t stack_size = 0;
};

class PublisherBase : public std::enable_shared_from_this<PublisherBase> {
public:
  PublisherBase(Module &module, subspace::Publisher pub,
                PublisherOptions options);
  virtual ~PublisherBase() = default;

  // Stop the callback publisher coroutine.
  void Stop();

  // Get a message buffer from subspace.  If the void* returned is nullptr
  // it means that a reliable channel buffer could not be obtained at this
  // time.  Try again later in that case.
  absl::StatusOr<void *> GetMessageBuffer(size_t size, co::Coroutine *c);

protected:
  template <typename T, typename L, typename S>
  friend class SerializingPublisher;
  template <typename T, typename C> friend class ZeroCopyPublisher;

  void BackpressureSubscribers();
  void ReleaseSubscribers();

  Module &module_;
  subspace::Publisher pub_;
  PublisherOptions options_;
  toolbelt::TriggerFd trigger_;
  std::string coroutine_name_;
  bool running_ = false;
  int pending_count_ = 0;
};

// Publisher publishes a serialized message into a Subspace publisher.  It
// can be used with either a callback function or by calling the Publish
// function with a pre-filled message.  The message is serialized into
// a Subspace buffer, therefore there is a copy of the message contents
// made.
template <typename MessageType, typename SerializedLength, typename Serialize>
class SerializingPublisher : public PublisherBase {
public:
  SerializingPublisher(Module &module, subspace::Publisher pub,
                       PublisherOptions options,
                       std::function<bool(std::shared_ptr<SerializingPublisher>,
                                          MessageType &, co::Coroutine *)>
                           callback)
      : SerializingPublisher<MessageType, SerializedLength, Serialize>(
            module, std::move(pub), std::move(options)) {
    callback_ = std::move(callback);
  }

  SerializingPublisher(Module &module, subspace::Publisher pub,
                       PublisherOptions options)
      : PublisherBase(module, std::move(pub), std::move(options)) {}

  // Publish message filled in by callback.  Callback must have been specified.
  // This can be used for both reliable and unreliable channels.  The callback
  // will be called when it is possible to try to send the message.
  void Publish() {
    pending_count_++;
    trigger_.Trigger();
  }

  // Publish a message directly.  A callback must not have been set.
  void Publish(const MessageType &msg, co::Coroutine *c) {
    assert(callback_ == nullptr);
    PublishMessageNow(msg, c);
  }

  // Run the callback publisher coroutine.
  void Run();

private:
  void PublishMessageNow(const MessageType &msg, co::Coroutine *c);

  std::function<bool(std::shared_ptr<SerializingPublisher>, MessageType &,
                     co::Coroutine *)>
      callback_;
};

// A ZeroCopyPublisher publishes a message into Subspace without copying the
// message.  You can pass a callback function that will be called with reference
// to the templated typed buffer that can be filled in by the callback.  You
// can also call the GetMessageBuffer and Publish functions to fill in a
// message outside of the callback.  The message is not protobuf and is not
// serialized or copied.
template <typename MessageType, typename Creator> class ZeroCopyPublisher : public PublisherBase {
public:
  ZeroCopyPublisher(Module &module, subspace::Publisher pub,
                    PublisherOptions options,
                    std::function<bool(std::shared_ptr<ZeroCopyPublisher>,
                                       MessageType &, co::Coroutine *)>
                        callback)
      : ZeroCopyPublisher<MessageType, Creator>(module, std::move(pub),
                                       std::move(options)) {
    callback_ = std::move(callback);
  }

  ZeroCopyPublisher(Module &module, subspace::Publisher pub,
                    PublisherOptions options)
      : PublisherBase(module, std::move(pub), std::move(options)) {}

  // Publish a message that is already intact in the message buffer.  The
  // buffer should be obtained by calling GetMessageBuffer.  The usage
  // model for this is:
  //
  // 1. Call GetMessageBuffer to get a buffer for the message
  // 2. Fill in the memory obtained from GetMessageBuffer
  // 3. Call this to publish the message in the buffer.
  //
  // There will be no copies of the message made.
  void Publish(size_t size, co::Coroutine *c) {
    absl::StatusOr<subspace::Message> msg = pub_.PublishMessage(size);
    if (!msg.ok()) {
      std::cerr << "Failed to publish buffer: " << msg.status().ToString()
                << std::endl;
      abort();
    }
  }

  // Publish message filled in by callback.  Callback must have been specified.
  // The callback will be invoked from the publisher's coroutine.
  void Publish() {
    pending_count_++;
    trigger_.Trigger();
  }

  void Run();

private:
  std::function<bool(std::shared_ptr<ZeroCopyPublisher>, MessageType &,
                     co::Coroutine *)>
      callback_;
};
} // namespace adastra::module
