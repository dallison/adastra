#pragma once
#include "absl/types/span.h"
#include "client/client.h"
#include "coroutine.h"
#include "google/protobuf/message.h"
#include "toolbelt/triggerfd.h"

namespace stagezero::module {

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
} // namespace stagezero::module
