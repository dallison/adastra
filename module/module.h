// Copyright 2024 David Allison
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

#include "module/message.h"
#include "module/publisher.h"
#include "module/subscriber.h"

// A module is something that is spawned from a zygote and communicates
// with other modules using subspace IPC.  Modules are templated types
// that work with any serialization mechanisms and also support zero-copy
// usage.
//
// To define a module, derive a type from adastra::module::Module.  Modules
// register publishers and subscribers during their initialization or any
// time after.  Publishers publish message and subscribers received messages
// published to subspace channels.
//
// Modules are forked from zygotes which allows them to be very light weight
// and share all the loaded code in the zygote.  You can define your own
// zygote to load up all the code you want to use for your modules (things
// like tensorflow or computer vision shared libraries).  This way every
// module that uses that zygote doesn't have to load that common code.
//
// Coroutines
// ----------
// This module system makes use of coroutines to allow cooperative parallelism
// in a single thread program.  This is not a thread-safe system so you could
// avoid threads wherever possible.  If you insist on using threads, it is
// up to you to make sure that there are no thread-safety issues.
//
// Coroutines are used to run the subscribers and publishers.  Each time you
// create a subscriber with a callback, a coroutine is created to listen
// for incoming messages on that subscriber and call the callback when one
// arrives.  Likewise for publishers that are created with a callback.  The
// program invokes the 'Publish' function that that will signal the coroutine
// to call the callback when it is possible to publish the message.  For
// unreliable publisher this will be almost immediately, but for reliable
// publishers it will depend on the current state of the channel.
//
// The callback functions are passed a pointer to a co::Coroutine.  This is
// not owned by the callback and can be ignored unless you want to do
// any blocking calls (like wait for a network socket, or sleep for a delay).
// If you do use blocking calls, make sure to use coroutine aware versions
// or use the Wait() functions of the coroutine to wait for input or output.
// Since this is single threaded, don't block the process as all other
// coroutines will also be blocked.
//
// By default the coroutines are created with a 32K stack.  If you need more
// stack space than this, you can used
namespace adastra::module {

class Module;

// User defined literals for frequency for the RunPeriodically function.
// Not really needed but makes the caller easier to read.
namespace frequency_literals {
constexpr long double operator"" _hz(long double f) { return f; }
constexpr long double operator"" _khz(long double f) { return f * 1000; }
constexpr long double operator"" _mhz(long double f) { return f * 1000000; }
constexpr long double operator"" _hz(unsigned long long f) { return f; }
constexpr long double operator"" _khz(unsigned long long f) { return f * 1000.0; }
constexpr long double operator"" _mhz(unsigned long long f) { return f * 1000000.0; }
} // namespace frequency_literals

class Module {
public:
  Module(std::unique_ptr<adastra::stagezero::SymbolTable> symbols);
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

  // Modules are passed a symbol table that contains variables.
  // The symbol table is prepopulated with some symbols from
  // capcom or flight director but you can also add you own.
  // The symbol table is accessible from the protected member
  // symbols_.
  const std::string &LookupSymbol(const std::string &name) const;

  // Register a serializing subscriber that will call the callback
  // function when a message is received on the given channel.  The
  // message will be deserialized before the callback is called.
  // The callback is called on a coroutine that is owned by the
  // subscriber.
  //
  // This overload allows you to specify options for the subscriber.
  template <typename MessageType, typename Deserialize>
  absl::StatusOr<
      std::shared_ptr<SerializingSubscriber<MessageType, Deserialize>>>
  RegisterSerializingSubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<
          void(std::shared_ptr<SerializingSubscriber<MessageType, Deserialize>>,
               Message<const MessageType>, co::Coroutine *)>
          callback);

  // Register a serializing subscriber that will call the callback
  // function when a message is received on the given channel.  The
  // message will be deserialized before the callback is called.
  // The callback is called on a coroutine that is owned by the
  // subscriber.
  //
  // This overload uses the default subscriber options.
  template <typename MessageType, typename Deserialize>
  absl::StatusOr<
      std::shared_ptr<SerializingSubscriber<MessageType, Deserialize>>>
  RegisterSerializingSubscriber(
      const std::string &channel,
      std::function<
          void(std::shared_ptr<SerializingSubscriber<MessageType, Deserialize>>,
               Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSerializingSubscriber(channel, {}, std::move(callback));
  }

  // Register a zero-copy subscriber with a callback that will be called
  // when a message is received on the given channel.  The message will
  // be delivered intact with no deserialization applied to it.  The
  // callback will be invoked on a coroutine owned by the subscriber.
  //
  // This overload provides a set of options for the subscriber.
  //
  // You can use "auto" for the arguments of a lambda for the callback
  // function if you don't like repeating all the template arguments.
  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ZeroCopySubscriber<MessageType>>>
  RegisterZeroCopySubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<void(std::shared_ptr<ZeroCopySubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback);

  // Register a zero-copy subscriber with a callback that will be called
  // when a message is received on the given channel.  The message will
  // be delivered intact with no deserialization applied to it.  The
  // callback will be invoked on a coroutine owned by the subscriber.
  //
  // This overload uses the default subscriber options.
  //
  // You can use "auto" for the arguments of a lambda for the callback
  // function if you don't like repeating all the template arguments.
  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ZeroCopySubscriber<MessageType>>>
  RegisterZeroCopySubscriber(
      const std::string &channel,
      std::function<void(std::shared_ptr<ZeroCopySubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterZeroCopySubscriber(channel, {}, std::move(callback));
  }

  // Register a serializing publisher with a callback that will be called
  // to fill in the message to be published.  The callback function will
  // be called when the 'Publish' function is called on the publisher.
  // This is an asynchronous operation the will invoke the callback in
  // a coroutine owned by the publisher.  The callback can return false
  // to prevent the message from being published.
  //
  // This works for both reliable and unreliable publishers.
  //
  // This overload allows publisher options to be set.
  //
  // NOTE: When registering a publisher with a callback, you can use "auto"
  // for the arguments of the lambda, but if you do you MUST use "auto&" for
  // the 'msg' argument otherwise the lambda will get a copy of the message
  // and that won't work.
  template <typename MessageType, typename SerializedLength, typename Serialize>
  absl::StatusOr<std::shared_ptr<
      SerializingPublisher<MessageType, SerializedLength, Serialize>>>
  RegisterSerializingPublisher(
      const std::string &channel, int slot_size, int num_slots,
      const PublisherOptions &options,
      std::function<bool(std::shared_ptr<SerializingPublisher<
                             MessageType, SerializedLength, Serialize>>,
                         MessageType &, co::Coroutine *)>
          callback);

  // Register a serializing publisher with a callback that will be called
  // to fill in the message to be published.  The callback function will
  // be called when the 'Publish' function is called on the publisher.
  // This is an asynchronous operation the will invoke the callback in
  // a coroutine owned by the publisher.  The callback can return false
  // to prevent the message from being published.
  //
  // This works for both reliable and unreliable publishers.
  //
  // This overload uses the default publisher options.
  //
  // NOTE: When registering a publisher with a callback, you can use "auto"
  // for the arguments of the lambda, but if you do you MUST use "auto&" for
  // the 'msg' argument otherwise the lambda will get a copy of the message
  // and that won't work.
  template <typename MessageType, typename SerializedLength, typename Serialize>
  absl::StatusOr<std::shared_ptr<
      SerializingPublisher<MessageType, SerializedLength, Serialize>>>
  RegisterSerializingPublisher(
      const std::string &channel, int slot_size, int num_slots,
      std::function<bool(std::shared_ptr<SerializingPublisher<
                             MessageType, SerializedLength, Serialize>>,
                         MessageType &, co::Coroutine *)>
          callback) {
    return RegisterSerializingPublisher(channel, slot_size, num_slots, {},
                                        callback);
  }

  // These two functions register serializing publishers that do not have
  // a callback function.  They cannot be used for reliable channels and must
  // be invoked using the 'Publish(const Message&, co::Coroutine*)' function
  // in the publisher.
  //
  // The first function allows options to be specified.
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

  // Zero-copy publishers behave in the same way as serializing publishers
  // except they do not use a serialization mechanism but instead pass the
  // message in an already-allocated subspace buffer.  The message has not been
  // initialized in any way and it is up the to the publisher to create whatever
  // structures are necessary in the buffer to complete the message.  In other
  // words, the publishers are given an empty buffer that needs to be filled
  // in.
  //
  // Both callback and non-callback overloads are provided with the same
  // usage and constraints as the serializing publisher functions.
  //
  // NOTE: When registering a publisher with a callback, you can use "auto"
  // for the arguments of the lambda, but if you do you MUST use "auto&" for
  // the 'msg' argument otherwise the lambda will get a copy of the message
  // and that won't work.
  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ZeroCopyPublisher<MessageType>>>
  RegisterZeroCopyPublisher(
      const std::string &channel, int slot_size, int num_slots,
      const PublisherOptions &options,
      std::function<bool(std::shared_ptr<ZeroCopyPublisher<MessageType>>,
                         MessageType &, co::Coroutine *)>
          callback);

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ZeroCopyPublisher<MessageType>>>
  RegisterZeroCopyPublisher(
      const std::string &channel, int slot_size, int num_slots,
      std::function<bool(std::shared_ptr<ZeroCopyPublisher<MessageType>>,
                         MessageType &, co::Coroutine *)>
          callback) {
    return RegisterZeroCopyPublisher(channel, slot_size, num_slots, {},
                                     callback);
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

  // Remove a subscriber or publisher.
  void RemoveSubscriber(const std::shared_ptr<SubscriberBase> sub);
  void RemovePublisher(const std::shared_ptr<PublisherBase> pub);

  void RemoveSubscriber(SubscriberBase &sub);
  void RemovePublisher(PublisherBase &pub);

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

  void AddSubscriber(std::shared_ptr<SubscriberBase> sub) {
    subscribers_.push_back(sub);
  }

  void AddPublisher(std::shared_ptr<PublisherBase> pub) {
    publishers_.push_back(pub);
  }

  std::unique_ptr<adastra::stagezero::SymbolTable> symbols_;
  subspace::Client subspace_client_;
  char *argv0_;

  // All coroutines are owned by this set.
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> coroutines_;

  std::list<std::shared_ptr<SubscriberBase>> subscribers_;
  std::list<std::shared_ptr<PublisherBase>> publishers_;

  co::CoroutineScheduler scheduler_;
}; // namespace adastra::module

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
            sub->callback_(sub, Message<const MessageType>(deserialized), c);
          }
        }
      },
      coroutine_name_, options_.stack_size);
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

            sub->callback_(sub, Message(std::move(shared_msg)), c);
          }
        }
      },
      coroutine_name_, options_.stack_size);
  module_.AddCoroutine(std::unique_ptr<co::Coroutine>(runner));
}

template <typename MessageType, typename Deserialize>
inline absl::StatusOr<
    std::shared_ptr<SerializingSubscriber<MessageType, Deserialize>>>
Module::RegisterSerializingSubscriber(
    const std::string &channel, const SubscriberOptions &options,
    std::function<
        void(std::shared_ptr<SerializingSubscriber<MessageType, Deserialize>>,
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

  AddSubscriber(sub);

  // Run a coroutine to read from the subscriber and call the callback for every
  // message received.
  sub->Run();

  return sub;
}

template <typename MessageType>
inline absl::StatusOr<std::shared_ptr<ZeroCopySubscriber<MessageType>>>
Module::RegisterZeroCopySubscriber(
    const std::string &channel, const SubscriberOptions &options,
    std::function<void(std::shared_ptr<ZeroCopySubscriber<MessageType>>,
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
      subspace_client_.CreateSubscriber(channel,
                                        {.reliable = options.reliable,
                                         .type = options.type,
                                         .max_shared_ptrs = max_shared_ptrs});
  if (!subspace_sub.ok()) {
    return subspace_sub.status();
  }
  auto sub = std::make_shared<ZeroCopySubscriber<MessageType>>(
      *this, std::move(*subspace_sub), std::move(options), std::move(callback));

  AddSubscriber(sub);

  // Run a coroutine to read from the subscriber and call the callback for every
  // message received.
  sub->Run();

  return sub;
}

template <typename MessageType, typename SerializedLength, typename Serialize>
inline void
SerializingPublisher<MessageType, SerializedLength,
                     Serialize>::PublishMessageNow(const MessageType &msg,
                                                   co::Coroutine *c) {
  int64_t length = SerializedLength::Invoke(msg);
  absl::StatusOr<void *> buffer = GetMessageBuffer(length, c);
  if (!buffer.ok()) {
    std::cerr << "Failed to get buffer: " << buffer.status().ToString()
              << std::endl;
    abort();
  }
  if (*buffer == nullptr) {
    // We have already checked that we can get a buffer before calling this
    // so if we fail here, it's an error
    std::cerr << "Failed to get reliable buffer pointer\n";
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
  return;
}

template <typename MessageType, typename SerializedLength, typename Serialize>
inline void
SerializingPublisher<MessageType, SerializedLength, Serialize>::Run() {
  co::Coroutine *runner = new co::Coroutine(
      module_.scheduler_,
      [pub = this->shared_from_this()](co::Coroutine * c) {
        pub->running_ = true;
        while (pub->running_) {
          // Wait for a trigger to allow us to try to publish.
          c->Wait({pub->trigger_.GetPollFd().Fd()}, POLLIN);
          pub->trigger_.Clear();
          while (pub->running_ && pub->pending_count_ > 0) {
            MessageType msg;
            if (pub->options_.reliable) {
              // For a reliable message we ask for a buffer before we call the
              // callback to fill in the message.  If we can't get a buffer we
              // go back to waiting for the chance to try again.  We ask
              // for the current slot size since we don't know the actual
              // serialized size of the message.  The PublishMessageNow function
              // does know the actual size of the message to send so it will
              // call GetMessageBuffer again and this will cause a resize to
              // occur if it's bigger than the current buffer size.
              absl::StatusOr<void *> buffer =
                  pub->GetMessageBuffer(pub->pub_.SlotSize(), c);
              if (!buffer.ok()) {
                std::cerr << "Failed to get buffer for reliable message: "
                          << buffer.status().ToString() << std::endl;
                abort();
              }
              if (*buffer == nullptr) {
                // Failed to allocate a buffer.  Can only happen on reliable
                // channels.
                break;
              }
            }
            std::shared_ptr<
                SerializingPublisher<MessageType, SerializedLength, Serialize>>
                self = std::static_pointer_cast<SerializingPublisher<
                    MessageType, SerializedLength, Serialize>>(pub);
            bool publish = self->callback_(self, msg, c);
            if (publish) {
              self->PublishMessageNow(msg, c);
            }
            pub->pending_count_--;
          }
        }
      },
      coroutine_name_, options_.stack_size);
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
                self, *reinterpret_cast<MessageType *>(*buffer), c);
            if (publish) {
              self->Publish(pub->pub_.SlotSize(), c);
            }
          }
        }
      },
      coroutine_name_, options_.stack_size);
  module_.AddCoroutine(std::unique_ptr<co::Coroutine>(runner));
}

template <typename MessageType, typename SerializedLength, typename Serialize>
inline absl::StatusOr<std::shared_ptr<
    SerializingPublisher<MessageType, SerializedLength, Serialize>>>
Module::RegisterSerializingPublisher(
    const std::string &channel, int slot_size, int num_slots,
    const PublisherOptions &options,
    std::function<bool(std::shared_ptr<SerializingPublisher<
                           MessageType, SerializedLength, Serialize>>,
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

  auto pub = std::make_shared<ZeroCopyPublisher<MessageType>>(
      *this, std::move(*subspace_pub), std::move(options));
  AddPublisher(pub);
  return pub;
}

template <typename MessageType>
inline absl::StatusOr<std::shared_ptr<ZeroCopyPublisher<MessageType>>>
Module::RegisterZeroCopyPublisher(
    const std::string &channel, int slot_size, int num_slots,
    const PublisherOptions &options,
    std::function<bool(std::shared_ptr<ZeroCopyPublisher<MessageType>>,
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

  auto pub = std::make_shared<ZeroCopyPublisher<MessageType>>(
      *this, std::move(*subspace_pub), std::move(options), std::move(callback));
  AddPublisher(pub);

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
    auto symbols = std::make_unique<adastra::stagezero::SymbolTable>();                 \
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

} // namespace adastra::module
