// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/debugging/failure_signal_handler.h"
#include "absl/debugging/symbolize.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "google/protobuf/message.h"
#include "stagezero/symbols.h"

#include "client/client.h"
#include "coroutine.h"
#include "toolbelt/triggerfd.h"
#include <assert.h>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <variant>

namespace stagezero::module {

class Module;

// User defined literals for frequency for the RunPeriodically function.
// Not really needed but makes the caller easier to read.
namespace frequency_literals {
constexpr long double operator"" _hz(long double f) { return f; }
constexpr long double operator"" _khz(long double f) { return f * 1000; }
constexpr long double operator"" _mhz(long double f) { return f * 1000000; }
constexpr long double operator"" _hz(uint64_t f) { return f; }
constexpr long double operator"" _khz(uint64_t f) { return f * 1000.0; }
constexpr long double operator"" _mhz(uint64_t f) { return f * 1000000.0; }
} // namespace frequency_literals

// This is a message received from IPC.  It is either a pointer to
// a deserialized protobuf message or a pointer to a message held
// in an IPC slot (as a subspace::shared_ptr).
template <typename MessageType> class Message {
public:
  Message() = default;
  Message(std::shared_ptr<MessageType> msg) : msg_(std::move(msg)) {}
  Message(subspace::shared_ptr<MessageType> msg) : msg_(std::move(msg)) {}
  ~Message() = default;
  MessageType *operator->() const {
    switch (msg_.index()) {
    case 0:
      return std::get<0>(msg_).get();
    case 1:
      return std::get<1>(msg_).get();
    }
    return nullptr;
  }

  MessageType *operator->() {
    switch (msg_.index()) {
    case 0:
      return std::get<0>(msg_).get();
    case 1:
      return std::get<1>(msg_).get();
    }
    return nullptr;
  }

  MessageType &operator*() const {
    static MessageType empty;
    switch (msg_.index()) {
    case 0:
      return *std::get<0>(msg_);
    case 1:
      return *std::get<1>(msg_);
    }
    return empty;
  }

  MessageType &operator*() {
    static MessageType empty;
    switch (msg_.index()) {
    case 0:
      return *std::get<0>(msg_);
    case 1:
      return *std::get<1>(msg_);
    }
    return empty;
  }

  MessageType *get() const {
    switch (msg_.index()) {
    case 0:
      return std::get<0>(msg_).get();
    case 1:
      return std::get<1>(msg_).get();
    }
    return nullptr;
  }

  operator absl::Span<MessageType>() {
    if (msg_.index() == 1) {
      const auto &m = std::get<1>(msg_).GetMessage();
      return absl::Span<MessageType>(reinterpret_cast<MessageType *>(m.buffer),
                                     m.length);
    }
    return absl::Span<MessageType>();
  }

  bool operator==(std::nullptr_t) {
    switch (msg_.index()) {
    case 0:
      return std::get<0>(msg_).get() == nullptr;
    case 1:
      return std::get<1>(msg_).get() == nullptr;
    }
    return false;
  }

  bool operator!=(std::nullptr_t) { return !(*this == nullptr); }

  bool operator==(const Message<MessageType> &m) {
    if (msg_.index() != m.index()) {
      return false;
    }
    switch (msg_.index()) {
    case 0:
      return std::get<0>(msg_) == std::get<0>(m);
    case 1:
      return std::get<1>(msg_) == std::get<0>(m);
    }
    return false;
  }

  bool operator!=(const Message<MessageType> &m) { return !(*this == m); }

  void reset() {
    switch (msg_.index()) {
    case 0:
      std::get<0>(msg_).reset();
      break;
    case 1:
      std::get<1>(msg_).reset();
      break;
    }
  }

private:
  std::variant<std::shared_ptr<MessageType>, subspace::shared_ptr<MessageType>>
      msg_;
};

struct SubscriberOptions {
  bool reliable = false;
  std::string type;
  int max_shared_ptrs = 0;
  subspace::ReadMode read_mode = subspace::ReadMode::kReadNext;
};

class SubscriberBase {
public:
  SubscriberBase(Module &module, subspace::Subscriber sub,
                 SubscriberOptions options);
  virtual ~SubscriberBase() = default;
  virtual void Run() = 0;
  void Stop() { trigger_.Trigger(); }

protected:
  template <typename T> friend class ZeroCopySubscriber;

  Module &module_;
  subspace::Subscriber sub_;
  SubscriberOptions options_;
  toolbelt::TriggerFd trigger_;
  std::string coroutine_name_;
};

// A Subscriber calls a callback when a message arrives on the Subspace channel.
// It passes the message as a reference to the templated type.  The message is
// a serialized message that is deserialized before being passed to the
// callback.
template <typename MessageType, typename Deserialize>
class SerializingSubscriber
    : public SubscriberBase,
      public std::enable_shared_from_this<
          SerializingSubscriber<MessageType, Deserialize>> {
public:
  SerializingSubscriber(
      Module &module, subspace::Subscriber sub, SubscriberOptions options,
      std::function<void(const SerializingSubscriber &,
                         Message<const MessageType>, co::Coroutine *)>
          callback)
      : SubscriberBase(module, std::move(sub), std::move(options)),
        callback_(std::move(callback)) {}

  void Run() override;

protected:
  std::function<void(const SerializingSubscriber &, Message<const MessageType>,
                     co::Coroutine *)>
      callback_;
};

// A ZeroCopySubscriber calls the callback function with a pointer to the
// Subspace buffer holding the message data.  The message is not deserialized,
// but instead a reference to the template typed
// message is passed intact.  The message is passed as a subspace::shared_ptr
// and should be converted to a subspace::weak_ptr before being stored if you
// want to avoid holding onto a message slot, preventing a publisher from using
// it.
template <typename MessageType>
class ZeroCopySubscriber
    : public SubscriberBase,
      public std::enable_shared_from_this<ZeroCopySubscriber<MessageType>> {
public:
  ZeroCopySubscriber(
      Module &module, subspace::Subscriber sub, SubscriberOptions options,
      std::function<void(const ZeroCopySubscriber &, Message<const MessageType>,
                         co::Coroutine *)>
          callback)
      : SubscriberBase(module, std::move(sub), std::move(options)),
        callback_(std::move(callback)) {}

  void Run() override;

private:
  std::function<void(const ZeroCopySubscriber &, Message<const MessageType>,
                     co::Coroutine *)>
      callback_;
};

struct PublisherOptions {
  bool local = false;
  bool reliable = false;
  bool fixed_size = false;
  std::string type;
  std::vector<std::shared_ptr<SubscriberBase>> backpressured_subscribers;
};

class PublisherBase : public std::enable_shared_from_this<PublisherBase> {
public:
  PublisherBase(Module &module, subspace::Publisher pub,
                PublisherOptions options);
  virtual ~PublisherBase() = default;

  // Stop the callback publisher coroutine.
  void Stop();

  absl::StatusOr<void *> GetMessageBuffer(size_t size, co::Coroutine *c);

protected:
  template <typename T, typename L, typename S>
  friend class SerializingPublisher;
  template <typename T> friend class ZeroCopyPublisher;

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
                       std::function<bool(const SerializingPublisher &,
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
  void Publish() {
    pending_count_++;
    trigger_.Trigger();
  }

  // Publish a message directly.  A callback must not have been set.
  void Publish(const MessageType &msg, co::Coroutine *c) {
    assert(callback_ == nullptr);
    PublishMessage(msg, c);
  }

  // Run the callback publisher coroutine.
  void Run();

private:
  void PublishMessage(const MessageType &msg, co::Coroutine *c);

  std::function<bool(const SerializingPublisher &, MessageType &,
                     co::Coroutine *)>
      callback_;
};

// A ZeroCopyPublisher publishes a message into Subspace without copying the
// message.  You can pass a callback function that will be called with reference
// to the templated typed buffer that can be filled in by the callback.  You
// can also call the GetMessageBuffer and Publish functions to fill in a
// message outside of the callback.  The message is not protobuf and is not
// serialized or copied.
template <typename MessageType> class ZeroCopyPublisher : public PublisherBase {
public:
  ZeroCopyPublisher(Module &module, subspace::Publisher pub,
                    PublisherOptions options,
                    std::function<bool(const ZeroCopyPublisher &, MessageType &,
                                       co::Coroutine *)>
                        callback)
      : ZeroCopyPublisher<MessageType>(module, std::move(pub),
                                       std::move(options)) {
    callback_ = std::move(callback);
  }

  ZeroCopyPublisher(Module &module, subspace::Publisher pub,
                    PublisherOptions options)
      : PublisherBase(module, std::move(pub), std::move(options)) {}

  // Publish a message that is already intact in the message buffer.  The
  // buffer can be obtained by calling GetMessageBuffer.
  void Publish(size_t size, co::Coroutine *c) {
    absl::StatusOr<subspace::Message> msg = pub_.PublishMessage(size);
    if (!msg.ok()) {
      std::cerr << "Failed to publish buffer: " << msg.status().ToString()
                << std::endl;
      abort();
    }
  }

  // Publish message filled in by callback.  Callback must have been specified.
  void Publish() {
    pending_count_++;
    trigger_.Trigger();
  }

  // Publish a message directly.  A callback must not have been set.
  void Publish(const MessageType &msg, co::Coroutine *c) {
    assert(callback_ == nullptr);
    PublishMessage(msg, c);
  }

  void Run();

private:
  void PublishMessage(const MessageType &msg, co::Coroutine *c);

  std::function<bool(const ZeroCopyPublisher &, MessageType &, co::Coroutine *)>
      callback_;
};

class Module {
public:
  Module(std::unique_ptr<stagezero::SymbolTable> symbols);
  virtual ~Module() = default;

  absl::Status ModuleInit();

  // Perform module-specific initialization.  Here is where you register
  // your publishers, subscribers and Run... functions.  The Run()
  // function will actually run your module.
  virtual absl::Status Init(int argc, char **argv) = 0;

  // Run the module as a set of coroutines.  Incoming messages will invoke
  // the callbacks.  You can use the Run... functions to invoke callbacks
  // using various stimuli.
  void Run();

  // Stops the module immediately.  All coroutines will be stopped in their
  // tracks.
  void Stop();

  const std::string &Name() const;
  const std::string &SubspaceSocket() const;

  const std::string &LookupSymbol(const std::string &name) const;

  template <typename MessageType, typename Deserialize>
  absl::StatusOr<
      std::shared_ptr<SerializingSubscriber<MessageType, Deserialize>>>
  RegisterSerializingSubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<
          void(const SerializingSubscriber<MessageType, Deserialize> &,
               Message<const MessageType>, co::Coroutine *)>
          callback);

  template <typename MessageType, typename Deserialize>
  absl::StatusOr<
      std::shared_ptr<SerializingSubscriber<MessageType, Deserialize>>>
  RegisterSerializingSubscriber(
      const std::string &channel,
      std::function<
          void(const SerializingSubscriber<MessageType, Deserialize> &,
               Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSerializingSubscriber(channel, {}, std::move(callback));
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ZeroCopySubscriber<MessageType>>>
  RegisterZeroCopySubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<void(const ZeroCopySubscriber<MessageType> &,
                         Message<const MessageType>, co::Coroutine *)>
          callback);

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ZeroCopySubscriber<MessageType>>>
  RegisterZeroCopySubscriber(
      const std::string &channel,
      std::function<void(const ZeroCopySubscriber<MessageType> &,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterZeroCopySubscriber(channel, {}, std::move(callback));
  }

  template <typename MessageType, typename SerializedLength, typename Serialize>
  absl::StatusOr<std::shared_ptr<
      SerializingPublisher<MessageType, SerializedLength, Serialize>>>
  RegisterSerializingPublisher(
      const std::string &channel, int slot_size, int num_slots,
      const PublisherOptions &options,
      std::function<bool(const SerializingPublisher<
                             MessageType, SerializedLength, Serialize> &,
                         MessageType &, co::Coroutine *)>
          callback);

  template <typename MessageType, typename SerializedLength, typename Serialize>
  absl::StatusOr<std::shared_ptr<
      SerializingPublisher<MessageType, SerializedLength, Serialize>>>
  RegisterSerializingPublisher(
      const std::string &channel, int slot_size, int num_slots,
      std::function<bool(const SerializingPublisher<
                             MessageType, SerializedLength, Serialize> &,
                         MessageType &, co::Coroutine *)>
          callback) {
    return RegisterSerializingPublisher(channel, slot_size, num_slots, {},
                                        callback);
  }

  // Publisher without callback, message is passed to Publish() directly.
  template <typename MessageType, typename SerializedLength, typename Serialize>
  absl::StatusOr<std::shared_ptr<
      SerializingPublisher<MessageType, SerializedLength, Serialize>>>
  RegisterSerializingPublisher(const std::string &channel, int slot_size,
                               int num_slots, const PublisherOptions &options);

  template <typename MessageType, typename SerializedLength, typename Serialize>
  absl::StatusOr<std::shared_ptr<
      SerializingPublisher<MessageType, SerializedLength, Serialize>>>
  RegisterSerializingPublisher(const std::string &channel, int slot_size,
                               int num_slots) {
    return RegisterSerializingPublisher<MessageType, SerializedLength,
                                        Serialize>(
        channel, slot_size, num_slots, PublisherOptions{});
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ZeroCopyPublisher<MessageType>>>
  RegisterZeroCopyPublisher(const std::string &channel, int slot_size,
                            int num_slots, const PublisherOptions &options);

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ZeroCopyPublisher<MessageType>>>
  RegisterZeroCopyPublisher(const std::string &channel, int slot_size,
                            int num_slots) {
    return RegisterZeroCopyPublisher<MessageType>(channel, slot_size, num_slots,
                                                  PublisherOptions{});
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ZeroCopyPublisher<MessageType>>>
  RegisterZeroCopyPublisher(
      const std::string &channel, int slot_size, int num_slots,
      const PublisherOptions &options,
      std::function<bool(const ZeroCopyPublisher<MessageType> &, MessageType &,
                         co::Coroutine *)>
          callback);

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ZeroCopyPublisher<MessageType>>>
  RegisterZeroCopyPublisher(
      const std::string &channel, int slot_size, int num_slots,
      std::function<bool(const ZeroCopyPublisher<MessageType> &, MessageType &,
                         co::Coroutine *)>
          callback) {
    return RegisterZeroCopyPublisher(channel, slot_size, num_slots, {},
                                     callback);
  }

  // Run the callback at a frequency in Hertz.
  // NOTE: you can use the user-defined literals for frequency if you like.
  //
  // For example: 1.5_hz or 1_khz.
  //
  // Like I said, not needed but kinda cute.  It would be better if C++ didn't
  // force you to use an underscore like in std::chrono_literals.  However, that
  // nicety is reserved for the standard library for some reason.
  void RunPeriodically(double frequency,
                       std::function<void(co::Coroutine *)> callback);

  // Run the callback once after a delay in nanoseconds.
  void RunAfterDelay(std::chrono::nanoseconds delay,
                     std::function<void(co::Coroutine *)> callback);

  // Run the callback immediately.
  void RunNow(std::function<void(co::Coroutine *)> callback);

  // Run the callback when the file descriptor is available for the poll_events.
  // The callback is passed the file descriptor.
  void RunOnEvent(int fd, std::function<void(int, co::Coroutine *)> callback,
                  short poll_events = POLLIN);

  // Run the callback when the file descriptor is available for the poll_evnets
  // or after a timeout in nanoseconds.  The callback is passed the file
  // descriptor of -1 if a timeout occurred.
  void RunOnEventWithTimeout(int fd, std::chrono::nanoseconds timeout,
                             std::function<void(int, co::Coroutine *)> callback,
                             short poll_events = POLLIN);

  // Run the callback when the file descriptor is available for the poll_events.
  // The callback is passed the file descriptor.
  void RunOnEvent(
      toolbelt::FileDescriptor fd,
      std::function<void(toolbelt::FileDescriptor, co::Coroutine *)> callback,
      short poll_events = POLLIN);

  // Run the callback when the file descriptor is available for the poll_evnets
  // or after a timeout in nanoseconds.  The callback is passed the file
  // descriptor of an invalid FileDescriptor (.Valid() == false) if a timeout
  // occurred.
  void RunOnEventWithTimeout(
      toolbelt::FileDescriptor fd, std::chrono::nanoseconds timeout,
      std::function<void(toolbelt::FileDescriptor, co::Coroutine *)> callback,
      short poll_events = POLLIN);

  absl::Status NotifyStartup();

protected:
  template <typename T, typename D> friend class SerializingSubscriber;
  template <typename T> friend class ZeroCopySubscriber;
  template <typename T, typename L, typename S>
  friend class SerializingPublisher;
  template <typename T> friend class ZeroCopyPublisher;
  friend class SubscriberBase;
  friend class PublisherBase;

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) {
    coroutines_.insert(std::move(c));
  }

  std::unique_ptr<stagezero::SymbolTable> symbols_;
  subspace::Client subspace_client_;
  char *argv0_;

  // All coroutines are owned by this set.
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> coroutines_;

  co::CoroutineScheduler scheduler_;
}; // namespace stagezero::module

template <typename MessageType, typename Deserialize>
inline void SerializingSubscriber<MessageType, Deserialize>::Run() {
  co::Coroutine *runner = new co::Coroutine(
      module_.scheduler_,
      [sub = this->shared_from_this()](co::Coroutine * c) {
        for (;;) {
          int fd = c->Wait({sub->sub_.GetFileDescriptor().Fd(),
                            sub->trigger_.GetPollFd().Fd()},
                           POLLIN);
          if (fd == sub->trigger_.GetPollFd().Fd()) {
            sub->trigger_.Clear();
            break;
          }
          for (;;) {
            absl::StatusOr<const subspace::Message> msg =
                sub->sub_.ReadMessage(sub->options_.read_mode);
            if (!msg.ok()) {
              // TODO Log an error here.
              return;
            }
            if (msg->length == 0) {
              break;
            }
            auto deserialized = std::make_shared<MessageType>();
            if (!Deserialize::Invoke(*deserialized, msg->buffer, msg->length)) {
              // TODO?
              continue;
            }
            sub->callback_(*sub, Message<const MessageType>(deserialized), c);
          }
        }
      },
      coroutine_name_);
  module_.AddCoroutine(std::unique_ptr<co::Coroutine>(runner));
}

template <typename MessageType>
inline void ZeroCopySubscriber<MessageType>::Run() {
  co::Coroutine *runner = new co::Coroutine(
      module_.scheduler_,
      [sub = this->shared_from_this()](co::Coroutine * c) {
        for (;;) {
          int fd = c->Wait({sub->sub_.GetFileDescriptor().Fd(),
                            sub->trigger_.GetPollFd().Fd()},
                           POLLIN);

          if (fd == sub->trigger_.GetPollFd().Fd()) {
            sub->trigger_.Clear();
            break;
          }
          for (;;) {
            absl::StatusOr<subspace::shared_ptr<const MessageType>> msg =
                sub->sub_.template ReadMessage<const MessageType>(
                    sub->options_.read_mode);
            if (!msg.ok()) {
              std::cerr << "Error reading zero-copy message on channel "
                        << sub->sub_.Name() << ": " << msg.status()
                        << std::endl;
              return;
            }
            auto shared_msg = std::move(*msg);
            if (!shared_msg) {
              break;
            }

            sub->callback_(*sub, Message(std::move(shared_msg)), c);
          }
        }
      },
      coroutine_name_);
  module_.AddCoroutine(std::unique_ptr<co::Coroutine>(runner));
}

template <typename MessageType, typename Deserialize>
inline absl::StatusOr<
    std::shared_ptr<SerializingSubscriber<MessageType, Deserialize>>>
Module::RegisterSerializingSubscriber(
    const std::string &channel, const SubscriberOptions &options,
    std::function<void(const SerializingSubscriber<MessageType, Deserialize> &,
                       Message<const MessageType>, co::Coroutine *)>
        callback) {
  absl::StatusOr<subspace::Subscriber> subspace_sub =
      subspace_client_.CreateSubscriber(
          channel, {.reliable = options.reliable,
                    .type = options.type,
                    .max_shared_ptrs = options.max_shared_ptrs});
  if (!subspace_sub.ok()) {
    return subspace_sub.status();
  }
  auto sub = std::make_shared<SerializingSubscriber<MessageType, Deserialize>>(
      *this, std::move(*subspace_sub), std::move(options), std::move(callback));

  // Run a coroutine to read from the subscriber and call the callback for every
  // message received.
  sub->Run();

  return sub;
}

template <typename MessageType>
inline absl::StatusOr<std::shared_ptr<ZeroCopySubscriber<MessageType>>>
Module::RegisterZeroCopySubscriber(
    const std::string &channel, const SubscriberOptions &options,
    std::function<void(const ZeroCopySubscriber<MessageType> &,
                       Message<const MessageType>, co::Coroutine *)>
        callback) {
  // Since we pass a shared pointer to the callback function, we
  // need one more than the user specifies.  If the user didn't
  // specify, we will need 2.
  int max_shared_ptrs = options.max_shared_ptrs;
  if (max_shared_ptrs == 0) {
    max_shared_ptrs = 1;
  }
  max_shared_ptrs++;

  absl::StatusOr<subspace::Subscriber> subspace_sub =
      subspace_client_.CreateSubscriber(
          channel, {.reliable = options.reliable,
                    .type = options.type,
                    .max_shared_ptrs = options.max_shared_ptrs});
  if (!subspace_sub.ok()) {
    return subspace_sub.status();
  }
  auto sub = std::make_shared<ZeroCopySubscriber<MessageType>>(
      *this, std::move(*subspace_sub), std::move(options), std::move(callback));

  // Run a coroutine to read from the subscriber and call the callback for every
  // message received.
  sub->Run();

  return sub;
}

template <typename MessageType, typename SerializedLength, typename Serialize>
inline void
SerializingPublisher<MessageType, SerializedLength, Serialize>::PublishMessage(
    const MessageType &msg, co::Coroutine *c) {
  int64_t length = SerializedLength::Invoke(msg);
  absl::StatusOr<void *> buffer = GetMessageBuffer(length, c);
  if (!buffer.ok()) {
    std::cerr << "Failed to get buffer: " << buffer.status().ToString()
              << std::endl;
    abort();
  }
  // We got a buffer, serialize the message into it and publish
  // it.
  if (!Serialize::Invoke(msg, *buffer, length)) {
    std::cerr << "Failed to serialize message" << std::endl;
    abort();
  }
  absl::StatusOr<subspace::Message> m = pub_.PublishMessage(length);
  if (!m.ok()) {
    std::cerr << "Failed to publish buffer: " << m.status().ToString()
              << std::endl;
    abort();
  }
}

template <typename MessageType, typename SerializedLength, typename Serialize>
inline void
SerializingPublisher<MessageType, SerializedLength, Serialize>::Run() {
  co::Coroutine *runner = new co::Coroutine(
      module_.scheduler_,
      [pub = this->shared_from_this()](co::Coroutine * c) {
        pub->running_ = true;
        while (pub->running_) {
          // Wait for a trigger to cause us to publish.
          c->Wait({pub->trigger_.GetPollFd().Fd()}, POLLIN);
          pub->trigger_.Clear();
          while (pub->running_ && pub->pending_count_ > 0) {
            pub->pending_count_--;
            MessageType msg;
            std::shared_ptr<
                SerializingPublisher<MessageType, SerializedLength, Serialize>>
                self = std::static_pointer_cast<SerializingPublisher<
                    MessageType, SerializedLength, Serialize>>(pub);
            bool publish = self->callback_(*self, msg, c);
            if (publish) {
              self->PublishMessage(msg, c);
            }
          }
        }
      },
      coroutine_name_);
  module_.AddCoroutine(std::unique_ptr<co::Coroutine>(runner));
}

template <typename MessageType>
inline void ZeroCopyPublisher<MessageType>::Run() {
  co::Coroutine *runner = new co::Coroutine(
      module_.scheduler_,
      [pub = this->shared_from_this()](co::Coroutine * c) {
        pub->running_ = true;
        while (pub->running_) {
          // Wait for a trigger to cause us to publish.
          c->Wait({pub->trigger_.GetPollFd().Fd()}, POLLIN);
          pub->trigger_.Clear();
          while (pub->running_ && pub->pending_count_ > 0) {
            pub->pending_count_--;
            absl::StatusOr<void *> buffer =
                pub->GetMessageBuffer(pub->pub_.SlotSize(), c);
            if (!buffer.ok()) {
              std::cerr << "Failed to get buffer: "
                        << buffer.status().ToString() << std::endl;
              abort();
            }
            std::shared_ptr<ZeroCopyPublisher<MessageType>> self =
                std::static_pointer_cast<ZeroCopyPublisher<MessageType>>(pub);
            bool publish = self->callback_(
                *self, *reinterpret_cast<MessageType *>(*buffer), c);
            if (publish) {
              self->Publish(pub->pub_.SlotSize(), c);
            }
          }
        }
      },
      coroutine_name_);
  module_.AddCoroutine(std::unique_ptr<co::Coroutine>(runner));
}

template <typename MessageType, typename SerializedLength, typename Serialize>
inline absl::StatusOr<std::shared_ptr<
    SerializingPublisher<MessageType, SerializedLength, Serialize>>>
Module::RegisterSerializingPublisher(
    const std::string &channel, int slot_size, int num_slots,
    const PublisherOptions &options,
    std::function<bool(
        const SerializingPublisher<MessageType, SerializedLength, Serialize> &,
        MessageType &, co::Coroutine *)>
        callback) {
  absl::StatusOr<subspace::Publisher> subspace_pub =
      subspace_client_.CreatePublisher(channel, slot_size, num_slots,
                                       {.local = options.local,
                                        .reliable = options.reliable,
                                        .fixed_size = options.fixed_size,
                                        .type = options.type});

  if (!subspace_pub.ok()) {
    return subspace_pub.status();
  }

  auto pub = std::make_shared<
      SerializingPublisher<MessageType, SerializedLength, Serialize>>(
      *this, std::move(*subspace_pub), std::move(options), std::move(callback));

  pub->Run();
  return pub;
}

template <typename MessageType, typename SerializedLength, typename Serialize>
inline absl::StatusOr<std::shared_ptr<
    SerializingPublisher<MessageType, SerializedLength, Serialize>>>
Module::RegisterSerializingPublisher(const std::string &channel, int slot_size,
                                     int num_slots,
                                     const PublisherOptions &options) {
  absl::StatusOr<subspace::Publisher> subspace_pub =
      subspace_client_.CreatePublisher(channel, slot_size, num_slots,
                                       {.local = options.local,
                                        .reliable = options.reliable,
                                        .fixed_size = options.fixed_size,
                                        .type = options.type});
  if (!subspace_pub.ok()) {
    return subspace_pub.status();
  }

  return std::make_shared<
      SerializingPublisher<MessageType, SerializedLength, Serialize>>(
      *this, std::move(*subspace_pub), std::move(options));
}

template <typename MessageType>
inline absl::StatusOr<std::shared_ptr<ZeroCopyPublisher<MessageType>>>
Module::RegisterZeroCopyPublisher(const std::string &channel, int slot_size,
                                  int num_slots,
                                  const PublisherOptions &options) {
  absl::StatusOr<subspace::Publisher> subspace_pub =
      subspace_client_.CreatePublisher(channel, slot_size, num_slots,
                                       {.local = options.local,
                                        .reliable = options.reliable,
                                        .fixed_size = options.fixed_size,
                                        .type = options.type});
  if (!subspace_pub.ok()) {
    return subspace_pub.status();
  }

  return std::make_shared<ZeroCopyPublisher<MessageType>>(
      *this, std::move(*subspace_pub), std::move(options));
}

template <typename MessageType>
inline absl::StatusOr<std::shared_ptr<ZeroCopyPublisher<MessageType>>>
Module::RegisterZeroCopyPublisher(
    const std::string &channel, int slot_size, int num_slots,
    const PublisherOptions &options,
    std::function<bool(const ZeroCopyPublisher<MessageType> &, MessageType &,
                       co::Coroutine *)>
        callback) {
  absl::StatusOr<subspace::Publisher> subspace_pub =
      subspace_client_.CreatePublisher(channel, slot_size, num_slots,
                                       {.local = options.local,
                                        .reliable = options.reliable,
                                        .fixed_size = options.fixed_size,
                                        .type = options.type});
  if (!subspace_pub.ok()) {
    return subspace_pub.status();
  }

  auto pub = std::make_shared<ZeroCopyPublisher<MessageType>>(
      *this, std::move(*subspace_pub), std::move(options), std::move(callback));

  pub->Run();
  return pub;
}

#define _DEFINE_MODULE(_type, _main)                                           \
  extern "C" void _main(const char *enc_syms, int syms_len, int argc,          \
                        char **argv) {                                         \
    absl::InitializeSymbolizer(argv[0]);                                       \
                                                                               \
    absl::InstallFailureSignalHandler({                                        \
        .use_alternate_stack = false,                                          \
    });                                                                        \
    auto symbols = std::make_unique<stagezero::SymbolTable>();                 \
    std::stringstream symstream;                                               \
    symstream.write(enc_syms, syms_len);                                       \
    symbols->Decode(symstream);                                                \
    std::unique_ptr<_type> module =                                            \
        std::make_unique<_type>(std::move(symbols));                           \
    /* Initialize the base module*/                                            \
    if (absl::Status status = module->ModuleInit(); !status.ok()) {            \
      std::cerr << "Failed to initialize base module " << module->Name()       \
                << ": " << status.ToString() << std::endl;                     \
      abort();                                                                 \
    }                                                                          \
    /* Now initialize the derived module.*/                                    \
    if (absl::Status status = module->Init(argc, argv); !status.ok()) {        \
      std::cerr << "Failed to initialize derived module " << module->Name()    \
                << ": " << status.ToString() << std::endl;                     \
      abort();                                                                 \
    }                                                                          \
    if (absl::Status status = module->NotifyStartup(); !status.ok()) {         \
      std::cerr << "Module startup failed " << status.ToString() << std::endl; \
      abort();                                                                 \
    }                                                                          \
    module->Run();                                                             \
  }

// Define a module that is in a DSO with the main function ModuleMain
#define DEFINE_MODULE(_type) _DEFINE_MODULE(_type, ModuleMain)

// Define a module that is embedded in the zygote with the main function
// given.
#define DEFINE_EMBEDDED_MODULE(_type, _main_func)                              \
  _DEFINE_MODULE(_type, _main_func)

} // namespace stagezero::module
