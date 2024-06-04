// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "davros/serdes/runtime.h"
#include "module/module.h"

// This is a module that uses Davros (custom ROS messages) as a serializer.
// The Publishers and Subscribers will serialize and deserialize their messages
// in the Subspace buffers and expose the messages to the program.

namespace adastra::module {

// Template function to calculate the serialized length of a ROS message.
template <typename MessageType> struct ROSSerializedLength {
  static uint64_t Invoke(const MessageType &msg) {
    return uint64_t(msg.SerializedLength());
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

// A ROS module is a module that uses the serializing publishers
// and subscribers to send and receive messages over Subspace.
// Despite all the C++ template syntax line noise, it's really a simple
// type wrapper around the Module's template functions.
class ROSModule : public Module {
public:
  ROSModule(std::unique_ptr<adastra::stagezero::SymbolTable> symbols)
      : Module(std::move(symbols)) {}

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ROSSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<void(std::shared_ptr<ROSSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSerializingSubscriber(channel, options, callback);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ROSSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel,
      std::function<void(std::shared_ptr<ROSSubscriber<MessageType>>,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSubscriber(channel, {}, std::move(callback));
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ROSPublisher<MessageType>>> RegisterPublisher(
      const std::string &channel, int slot_size, int num_slots,
      const PublisherOptions &options,
      std::function<bool(std::shared_ptr<ROSPublisher<MessageType>>,
                         MessageType &, co::Coroutine *)>
          callback) {
    PublisherOptions opts = options;
    opts.type = absl::StrFormat("ROS/%s", MessageType::FullName());
    return RegisterSerializingPublisher(channel, slot_size, num_slots, opts,
                                        callback);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ROSPublisher<MessageType>>> RegisterPublisher(
      const std::string &channel, int slot_size, int num_slots,
      std::function<bool(std::shared_ptr<ROSPublisher<MessageType>>,
                         MessageType &, co::Coroutine *)>
          callback) {
    PublisherOptions opts = {
        .type = absl::StrFormat("ROS/%s", MessageType::FullName())};
    return RegisterPublisher(channel, slot_size, num_slots, opts, callback);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ROSPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots,
                    const PublisherOptions &options) {
    return RegisterSerializingPublisher<MessageType,
                                        ROSSerializedLength<MessageType>,
                                        ROSSerialize<MessageType>>(
        channel, slot_size, num_slots, options);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ROSPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots) {
    return RegisterSerializingPublisher<MessageType,
                                        ROSSerializedLength<MessageType>,
                                        ROSSerialize<MessageType>>(
        channel, slot_size, num_slots);
  }
};
} // namespace adastra::module
