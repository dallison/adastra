#pragma once
#include "absl/types/span.h"
#include "client/client.h"
#include "google/protobuf/message.h"
#include <tuple>

namespace adastra::module {

// This is a message received from IPC.  It is a pointer to
// a deserialized protobuf message and/or a pointer to a message held
// in an IPC slot (as a subspace::shared_ptr).
template <typename MessageType> class Message {
public:
  Message() = default;
  Message(std::shared_ptr<MessageType> msg) {
    std::get<0>(msg_) = std::move(msg);
    index_ = 0;
 }
  Message(subspace::shared_ptr<MessageType> msg) {
    std::get<1>(msg_) = std::move(msg);
    index_ = 1;
  }
  // Holds both shared pointers to message.  If the std::shared_ptr
  // is nullptr we use the subspace::shared_ptr.
  Message(std::shared_ptr<MessageType> msg,
          subspace::shared_ptr<MessageType> smsg) {
    index_ = msg == nullptr ? 1 : 0;
    std::get<0>(msg_) = std::move(msg);
    std::get<1>(msg_) = std::move(smsg);
  }

  ~Message() = default;
  MessageType *operator->() const {
    switch (index_) {
    case 0:
      return std::get<0>(msg_).get();
    case 1:
      return std::get<1>(msg_).get();
    }
    return nullptr;
  }

  MessageType *operator->() {
   switch (index_) {
    case 0:
      return std::get<0>(msg_).get();
    case 1:
      return std::get<1>(msg_).get();
    default:
      return nullptr;
    }
  }

  MessageType &operator*() const {
   switch (index_) {
    case 0:
      return *std::get<0>(msg_);
    case 1:
      return *std::get<1>(msg_);
    }
  }

  MessageType &operator*() {
    switch (index_) {
    case 0:
      return *std::get<0>(msg_);
    case 1:
      return *std::get<1>(msg_);
    default:
      abort();
    }
  }

  MessageType *get() const {
    switch (index_) {
    case 0:
      return std::get<0>(msg_).get();
    case 1:
      return std::get<1>(msg_).get();
    }
    return nullptr;
  }

  operator absl::Span<MessageType>() {
    if (index_ == 1) {
      const auto &m = std::get<1>(msg_).GetMessage();
      return absl::Span<MessageType>(reinterpret_cast<MessageType *>(m.buffer),
                                     m.length);
    }
    return absl::Span<MessageType>();
  }

  bool operator==(std::nullptr_t) {
    switch (index_) {
    case 0:
      return std::get<0>(msg_).get() == nullptr;
    case 1:
      return std::get<1>(msg_).get() == nullptr;
    }
    return false;
  }

  bool operator!=(std::nullptr_t) { return !(*this == nullptr); }

  bool operator==(const Message<MessageType> &m) {
    if (index_ != m.index_) {
      return false;
    }
    switch (index_) {
    case 0:
      return std::get<0>(msg_) == std::get<0>(m);
    case 1:
      return std::get<1>(msg_) == std::get<1>(m);
    }
    return false;
  }

  bool operator!=(const Message<MessageType> &m) { return !(*this == m); }

  void reset() {
    std::get<0>(msg_).reset();
    std::get<1>(msg_).reset();
  }

private:
  template <typename T> friend class WeakMessage;
  std::tuple<std::shared_ptr<MessageType>, subspace::shared_ptr<MessageType>>
      msg_;
  int index_ = -1;
};

// This is a partially weak message.  The front-end message is not weakened but
// the Subspace shared_ptr is converted to a weak_ptr to allow reuse
// of the message slot.
template <typename MessageType> class WeakMessage {
public:
  WeakMessage() = default;
  WeakMessage(const Message<MessageType> &msg) : wptr_(std::get<0>(msg.msg_)), wslot_(std::get<1>(msg.msg_)) {
  }

  bool expired() const { return wslot_.expired(); }

  Message<MessageType> lock() const {
    return Message<MessageType>(wptr_,
                                std::move(wslot_.lock()));
  }

  bool operator==(const WeakMessage<MessageType> &m) {
    return wptr_ == m.wptr_ &&
           wslot_ == m.wslot_;
  }

  bool operator!=(const WeakMessage<MessageType> &m) { return !(*this == m); }

  void reset() {
    wptr_.reset();
    wslot_.reset();
  }

private:
  std::shared_ptr<MessageType> wptr_;
  subspace::weak_ptr<MessageType> wslot_;
};

} // namespace adastra::module
