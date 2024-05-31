#pragma once
#include "absl/types/span.h"
#include "client/client.h"
#include "coroutine.h"
#include "google/protobuf/message.h"
#include "toolbelt/triggerfd.h"

namespace adastra::module {

class Module;

struct SubscriberOptions {
  bool reliable = false;
  std::string type;
  int max_shared_ptrs = 0;
  subspace::ReadMode read_mode = subspace::ReadMode::kReadNext;
  size_t stack_size = 0;
};

class SubscriberBase {
public:
  SubscriberBase(Module &module, subspace::Subscriber sub,
                 SubscriberOptions options);
  virtual ~SubscriberBase() = default;
  virtual void Run() = 0;
  void Stop() { stop_trigger_.Trigger(); }

  void Backpressure() { backpressured_ = true; }
  void ReleaseBackpressure() { backpressured_ = false; }
  
  std::string Name() const { return sub_.Name(); }

protected:
  template <typename T, typename C> friend class ZeroCopySubscriber;

  Module &module_;
  subspace::Subscriber sub_;
  SubscriberOptions options_;
  toolbelt::TriggerFd stop_trigger_;
  std::string coroutine_name_;
  bool backpressured_ = false;
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
      std::function<void(std::shared_ptr<SerializingSubscriber>,
                         Message<const MessageType>, co::Coroutine *)>
          callback)
      : SubscriberBase(module, std::move(sub), std::move(options)),
        callback_(std::move(callback)) {}

  void Run() override;

protected:
  std::function<void(std::shared_ptr<SerializingSubscriber>,
                     Message<const MessageType>, co::Coroutine *)>
      callback_;
};

// Default creator for ZeroCopySubscriber that returns nullptr.  This
// will make the subscriber see the data directly in the IPC buffer.
template <typename MessageType>
struct NullSubCreator {
    static std::shared_ptr<MessageType> Invoke(const void* buffer, size_t size) {
      return nullptr;
    }
};

// A ZeroCopySubscriber calls the callback function without deserializing
// the message.  There are two ways of doing this depending on how your
// zero-copy system works:
//
// 1. If you are using a simple byte array or POD struct, the callback will get
//    a pointer to the data inside the Subspace buffer.
// 2. If you are using a message system that splits the message between
//    a front-end and back-end, where the front-end contains information
//    about the message that is held in the back-end, you will get
//    a pointer to the front-end message and that will refer to the
//    back-end in the Subspace buffer.  The subspace buffer is held
//    as a subspace::shared_ptr which will prevent its reuse until
//    the callback returns.
//
// The configuration of each option is done using the 'Creator' template
// argument.  By default, this is set up to use option #1, but if you
// have a more complex system, you can create a custom Creator class that
// supplies a static function called 'Invoke' that takes a pointer to the
// buffer and the size of the buffer and returns a shared_ptr to the
// message.
//
// The signature for Invoke is:
// static std::shared_ptr<const MessageType> Invoke(const void* buffer, size_t size);
//
// This returns nullptr for option #1 or a std::shared_ptr to the front-end
// message for option #1.
//
// The default Creator is NullSubCreator which returns nullptr.
template <typename MessageType, typename Creator = NullSubCreator<MessageType>>
class ZeroCopySubscriber
    : public SubscriberBase,
      public std::enable_shared_from_this<ZeroCopySubscriber<MessageType, Creator>> {
public:
  ZeroCopySubscriber(
      Module &module, subspace::Subscriber sub, SubscriberOptions options,
      std::function<void(std::shared_ptr<ZeroCopySubscriber>,
                         Message<const MessageType>, co::Coroutine *)>
          callback)
      : SubscriberBase(module, std::move(sub), std::move(options)),
        callback_(std::move(callback)) {}

  void Run() override;

private:
  std::function<void(std::shared_ptr<ZeroCopySubscriber>,
                     Message<const MessageType>, co::Coroutine *)>
      callback_;
};
} // namespace adastra::module
