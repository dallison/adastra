#pragma once
#include <variant>
#include "absl/types/span.h"
#include "google/protobuf/message.h"
#include "client/client.h"

namespace stagezero::module {

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
}
