// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "neutron/serdes/runtime.h"
#include "neutron/zeros/runtime.h"
#include "module/module.h"

// This is a module that uses Davros (custom ROS messages) as a serializer.
// The Publishers and Subscribers will serialize and deserialize their messages
// in the Subspace buffers and expose the messages to the program.
//
// Also supports zero-copy ROS messages using Zeros.  Just use Zeros-generated
// messages and it will all work.
namespace adastra::module {

// Template function to calculate the serialized length of a ROS message.
template <typename MessageType> struct ROSSerializedLength {
  static uint64_t Invoke(const MessageType &msg) {
    return uint64_t(msg.SerializedSize());
  }
};

// Template function to serialize a ROS message to an array.
template <typename MessageType> struct ROSSerialize {
  static bool Invoke(const MessageType &msg, void *buffer, size_t buflen) {
    return msg.SerializeToArray(reinterpret_cast<char *>(buffer), buflen).ok();
  }
};

// Template function to calulate the deserialize a ROS message from
// an array.
template <typename MessageType> struct ROSDeserialize {
  static bool Invoke(MessageType &msg, const void *buffer, size_t buflen) {
    return msg
        .DeserializeFromArray(reinterpret_cast<const char *>(buffer), buflen)
        .ok();
  }
};

// Partial specialization for subscribers and publishers for ROS.
template <typename MessageType>
using ROSSubscriber =
    SerializingSubscriber<MessageType, ROSDeserialize<MessageType>>;

template <typename MessageType>
using ROSPublisher =
    SerializingPublisher<MessageType, ROSSerializedLength<MessageType>,
                         ROSSerialize<MessageType>>;

// Creator for mutable messages for Zeros publishers.
template <typename MessageType> struct ZerosPubCreator {
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

// Creator for readonly messages for Zeros subscribers.
template <typename MessageType> struct ZerosSubCreator {
  static std::shared_ptr<const MessageType> Invoke(const void *buffer,
                                                   size_t size) {
    return std::make_shared<MessageType>(
        MessageType::CreateReadonly(buffer, size));
  }
};

// Partial specialization for subscribers and publishers for phaser.
template <typename MessageType>
using ZerosSubscriber =
    ZeroCopySubscriber<MessageType, ZerosSubCreator<MessageType>>;

template <typename MessageType>
using ZerosPublisher =
    ZeroCopyPublisher<MessageType, ZerosPubCreator<MessageType>>;

// A ROS module is a module that uses the serializing publishers
// and subscribers to send and receive messages over Subspace.
// Despite all the C++ template syntax line noise, it's really a simple
// type wrapper around the Module's template functions.
class ROSModule : public virtual Module {
public:
  ROSModule() = default;

  template <typename MessageType,
            typename = std::enable_if_t<
                !std::is_base_of<::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ROSSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<void(std::shared_ptr<ROSSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSerializingSubscriber(channel, options, callback);
  }

  template <typename MessageType,
            typename = std::enable_if_t<
                !std::is_base_of<::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ROSSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel,
      std::function<void(std::shared_ptr<ROSSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSubscriber(channel, {}, std::move(callback));
  }

  template <typename MessageType,
            typename = std::enable_if_t<
                !std::is_base_of<::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ROSPublisher<MessageType>>> RegisterPublisher(
      const std::string &channel, int slot_size, int num_slots,
      const PublisherOptions &options,
      std::function<size_t(std::shared_ptr<ROSPublisher<MessageType>>,
                         MessageType &, co::Coroutine *)>
          callback) {
    PublisherOptions opts = options;
    opts.type = absl::StrFormat("ROS/%s", MessageType::FullName());
    return RegisterSerializingPublisher(channel, slot_size, num_slots, opts,
                                        callback);
  }

  template <typename MessageType,
            typename = std::enable_if_t<
                !std::is_base_of<::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ROSPublisher<MessageType>>> RegisterPublisher(
      const std::string &channel, int slot_size, int num_slots,
      std::function<size_t(std::shared_ptr<ROSPublisher<MessageType>>,
                         MessageType &, co::Coroutine *)>
          callback) {
    PublisherOptions opts = {
        .type = absl::StrFormat("ROS/%s", MessageType::FullName())};
    return RegisterPublisher(channel, slot_size, num_slots, opts, callback);
  }

  template <typename MessageType,
            typename = std::enable_if_t<
                !std::is_base_of<::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ROSPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots,
                    const PublisherOptions &options) {
    return RegisterSerializingPublisher<MessageType,
                                        ROSSerializedLength<MessageType>,
                                        ROSSerialize<MessageType>>(
        channel, slot_size, num_slots, options);
  }

  template <typename MessageType,
            typename = std::enable_if_t<
                !std::is_base_of<::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ROSPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots) {
    return RegisterSerializingPublisher<MessageType,
                                        ROSSerializedLength<MessageType>,
                                        ROSSerialize<MessageType>>(
        channel, slot_size, num_slots);
  }

  // Zeros (zero-copy support).
  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ZerosSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<void(std::shared_ptr<ZerosSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterZeroCopySubscriber(channel, options, callback);
  }

  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ZerosSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel,
      std::function<void(std::shared_ptr<ZerosSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSubscriber(channel, {}, std::move(callback));
  }

  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ZerosPublisher<MessageType>>>
  RegisterPublisher(
      const std::string &channel, int slot_size, int num_slots,
      const PublisherOptions &options,
      std::function<size_t(std::shared_ptr<ZerosPublisher<MessageType>>,
                           MessageType &, co::Coroutine *)>
          callback) {
    PublisherOptions opts = options;
    opts.type = absl::StrFormat("zeros/%s", MessageType::FullName());
    return RegisterZeroCopyPublisher(channel, slot_size, num_slots, opts,
                                     callback);
  }

  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ZerosPublisher<MessageType>>>
  RegisterPublisher(
      const std::string &channel, int slot_size, int num_slots,
      std::function<size_t(std::shared_ptr<ZerosPublisher<MessageType>>,
                           MessageType &, co::Coroutine *)>
          callback) {
    PublisherOptions opts = {
        .type = absl::StrFormat("zeros/%s", MessageType::FullName())};
    return RegisterPublisher(channel, slot_size, num_slots, opts,
                                   callback);
  }

  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ZerosPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots,
                    const PublisherOptions &options) {
    return RegisterZeroCopyPublisher<MessageType>(channel, slot_size, num_slots,
                                                  options);
  }

  template <typename MessageType, typename = std::enable_if_t<std::is_base_of<
                                      ::neutron::zeros::Message, MessageType>::value>>
  absl::StatusOr<std::shared_ptr<ZerosPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots) {
    return RegisterZeroCopyPublisher<MessageType>(channel, slot_size,
                                                  num_slots);
  }

};
} // namespace adastra::module
