// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "module/module.h"

// This is a module that uses Phaser to provide zero-copy protobuf
// messages

namespace adastra::module {

// Creator for mutable messages for publishers.
template <typename MessageType> struct PhaserPubCreator {
  static absl::StatusOr<MessageType> Invoke(subspace::Publisher &pub,
                                            size_t size) {
    return MessageType::CreateDynamicMutable(
        size,
        // Allocator for initial message.
        [&pub](size_t size) -> absl::StatusOr<void *> {
          return pub.GetMessageBuffer(size);
        },
        [](void *) {}, // Nothing to free.
        // Reallocator when we run out of memory in the buffer.
        [&pub](void *old_buffer, size_t old_size,
               size_t new_size) -> absl::StatusOr<void *> {
          absl::StatusOr<void *> buffer = pub.GetMessageBuffer(new_size);
          if (!buffer.ok()) {
            return buffer.status();
          }
          memcpy(*buffer, old_buffer, old_size);
          return *buffer;
        });
  }
};

// Creator for readonly messages for subscribers.
template <typename MessageType> struct PhaserSubCreator {
  static std::shared_ptr<const MessageType> Invoke(const void *buffer,
                                                   size_t size) {
    return std::make_shared<MessageType>(
        MessageType::CreateReadonly(buffer, size));
  }
};

// Partial specialization for subscribers and publishers for protobuf.
template <typename MessageType>
using PhaserSubscriber =
    ZeroCopySubscriber<MessageType, PhaserSubCreator<MessageType>>;

template <typename MessageType>
using PhaserPublisher =
    ZeroCopyPublisher<MessageType, PhaserPubCreator<MessageType>>;

class PhaserModule : public virtual Module {
public:
  PhaserModule() = default;

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<PhaserSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<void(std::shared_ptr<PhaserSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterZeroCopySubscriber(channel, options, callback);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<PhaserSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel,
      std::function<void(std::shared_ptr<PhaserSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSubscriber(channel, {}, std::move(callback));
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<PhaserPublisher<MessageType>>>
  RegisterPublisher(
      const std::string &channel, int slot_size, int num_slots,
      const PublisherOptions &options,
      std::function<size_t(std::shared_ptr<PhaserPublisher<MessageType>>,
                           MessageType &, co::Coroutine *)>
          callback) {
    PublisherOptions opts = options;
    opts.type = absl::StrFormat("phaser/%s", MessageType::FullName());
    return RegisterZeroCopyPublisher(channel, slot_size, num_slots, opts,
                                     callback);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<PhaserPublisher<MessageType>>>
  RegisterPublisher(
      const std::string &channel, int slot_size, int num_slots,
      std::function<size_t(std::shared_ptr<PhaserPublisher<MessageType>>,
                           MessageType &, co::Coroutine *)>
          callback) {
    PublisherOptions opts = {
        .type = absl::StrFormat("phaser/%s", MessageType::FullName())};
    return RegisterPublisher(channel, slot_size, num_slots, opts, callback);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<PhaserPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots,
                    const PublisherOptions &options) {
    return RegisterZeroCopyPublisher<MessageType>(channel, slot_size, num_slots,
                                                  options);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<PhaserPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots) {
    return RegisterZeroCopyPublisher<MessageType>(channel, slot_size,
                                                  num_slots);
  }
};
} // namespace adastra::module
