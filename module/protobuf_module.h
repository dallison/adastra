// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "google/protobuf/message.h"
#include "module/module.h"
#include "phaser/runtime/message.h"

// This is a module that uses Google's Protocol Buffers as a serializer.
// The Publishers and Subscribers will serialize and deserialize their messages
// in the Subspace buffers and expose the messages to the program.
//
// Also supported is zero-copy protobuf messages using Phaser.
namespace adastra::module {

// Template function to calculate the serialized length of a protobuf message.
template <typename MessageType> struct ProtobufSerializedLength {
  static uint64_t Invoke(const MessageType &msg) { return msg.ByteSizeLong(); }
};

// Template function to serialize a protobuf message to an array.
template <typename MessageType> struct ProtobufSerialize {
  static bool Invoke(const MessageType &msg, void *buffer, size_t buflen) {
    return msg.SerializeToArray(buffer, buflen);
  }
};

// Template function to calulate the deserialize a protobuf message from
// an array.
template <typename MessageType> struct ProtobufDeserialize {
  static bool Invoke(MessageType &msg, const void *buffer, size_t buflen) {
    return msg.ParseFromArray(buffer, buflen);
  }
};

// Partial specialization for subscribers and publishers for protobuf.
template <typename MessageType>
using ProtobufSubscriber =
    SerializingSubscriber<MessageType, ProtobufDeserialize<MessageType>>;

template <typename MessageType>
using ProtobufPublisher =
    SerializingPublisher<MessageType, ProtobufSerializedLength<MessageType>,
                         ProtobufSerialize<MessageType>>;

// Creator for mutable messages for Phaser publishers.
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

// Creator for readonly messages for Phaser subscribers.
template <typename MessageType> struct PhaserSubCreator {
  static std::shared_ptr<const MessageType> Invoke(const void *buffer,
                                                   size_t size) {
    return std::make_shared<MessageType>(
        MessageType::CreateReadonly(buffer, size));
  }
};

// Partial specialization for subscribers and publishers for phaser.
template <typename MessageType>
using PhaserSubscriber =
    ZeroCopySubscriber<MessageType, PhaserSubCreator<MessageType>>;

template <typename MessageType>
using PhaserPublisher =
    ZeroCopyPublisher<MessageType, PhaserPubCreator<MessageType>>;

// A protobuf module is a module that uses the serializing publishers
// and subscribers to send and receive messages over Subspace.
// Despite all the C++ template syntax line noise, it's really a simple
// type wrapper around the Module's template functions.
//
// This also supports zero-copy messages using Phaser.  To use this, just
// pass the appropriate Phaser message type as the template argument
// to the CreateSubscriber and CreatePublisher functions.
class ProtobufModule : public virtual Module {
public:
  ProtobufModule() = default;

  template <typename MessageType,
            typename = std::enable_if_t<std::is_base_of<
                ::google::protobuf::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ProtobufSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<void(std::shared_ptr<ProtobufSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSerializingSubscriber(channel, options, callback);
  }

  template <typename MessageType,
            typename = std::enable_if_t<std::is_base_of<
                ::google::protobuf::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ProtobufSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel,
      std::function<void(std::shared_ptr<ProtobufSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSubscriber(channel, {}, std::move(callback));
  }

  template <typename MessageType,
            typename = std::enable_if_t<std::is_base_of<
                ::google::protobuf::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ProtobufPublisher<MessageType>>>
  RegisterPublisher(
      const std::string &channel, int slot_size, int num_slots,
      const PublisherOptions &options,
      std::function<size_t(std::shared_ptr<ProtobufPublisher<MessageType>>,
                           MessageType &, co::Coroutine *)>
          callback) {
    PublisherOptions opts = options;
    const ::google::protobuf::Descriptor *desc = MessageType::descriptor();
    opts.type = absl::StrFormat("protobuf/%s", desc->full_name());
    return RegisterSerializingPublisher(channel, slot_size, num_slots, opts,
                                        callback);
  }

  template <typename MessageType,
            typename = std::enable_if_t<std::is_base_of<
                ::google::protobuf::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ProtobufPublisher<MessageType>>>
  RegisterPublisher(
      const std::string &channel, int slot_size, int num_slots,
      std::function<size_t(std::shared_ptr<ProtobufPublisher<MessageType>>,
                           MessageType &, co::Coroutine *)>
          callback) {
    const ::google::protobuf::Descriptor *desc = MessageType::descriptor();
    PublisherOptions opts = {
        .type = absl::StrFormat("protobuf/%s", desc->full_name())};
    return RegisterPublisher(channel, slot_size, num_slots, opts, callback);
  }

  template <typename MessageType,
            typename = std::enable_if_t<std::is_base_of<
                ::google::protobuf::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ProtobufPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots,
                    const PublisherOptions &options) {
    return RegisterSerializingPublisher<MessageType,
                                        ProtobufSerializedLength<MessageType>,
                                        ProtobufSerialize<MessageType>>(
        channel, slot_size, num_slots, options);
  }

  template <typename MessageType,
            typename = std::enable_if_t<std::is_base_of<
                ::google::protobuf::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ProtobufPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots) {
    return RegisterSerializingPublisher<MessageType,
                                        ProtobufSerializedLength<MessageType>,
                                        ProtobufSerialize<MessageType>>(
        channel, slot_size, num_slots);
  }

  // Phaser (zero-copy support).
  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::phaser::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<PhaserSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<void(std::shared_ptr<PhaserSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterZeroCopySubscriber(channel, options, callback);
  }

  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::phaser::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<PhaserSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel,
      std::function<void(std::shared_ptr<PhaserSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSubscriber(channel, {}, std::move(callback));
  }

  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::phaser::Message, MessageType>::value>>
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

  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::phaser::Message, MessageType>::value>>
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

  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::phaser::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<PhaserPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots,
                    const PublisherOptions &options) {
    return RegisterZeroCopyPublisher<MessageType>(channel, slot_size, num_slots,
                                                  options);
  }

  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::phaser::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<PhaserPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots) {
    return RegisterZeroCopyPublisher<MessageType>(channel, slot_size,
                                                  num_slots);
  }
};
} // namespace adastra::module
