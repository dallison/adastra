// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "module/module.h"

// This is a module that uses Google's Protocol Buffers as a serializer.
// The Publishers and Subscribers will serialize and deserialize their messages
// in the Subspace buffers and expose the messages to the program.

namespace stagezero::module {

template <typename MessageType> struct ProtobufSerializedLength {
  static uint64_t Invoke(const MessageType &msg) { return msg.ByteSizeLong(); }
};

template <typename MessageType> struct ProtobufSerialize {
  static uint64_t Invoke(const MessageType &msg, void *buffer, size_t buflen) {
    return msg.SerializeToArray(buffer, buflen);
  }
};

template <typename MessageType> struct ProtobufDeserialize {
  static uint64_t Invoke(MessageType &msg, const void *buffer, size_t buflen) {
    return msg.ParseFromArray(buffer, buflen);
  }
};

// Partial specialization for subscribers and publishers for protobuf.
template <typename MessageType>
using ProtobufSubscriber =
    Subscriber<MessageType, ProtobufDeserialize<MessageType>>;

template <typename MessageType>
using ProtobufPublisher =
    Publisher<MessageType, ProtobufSerializedLength<MessageType>,
              ProtobufSerialize<MessageType>>;


class ProtobufModule : public Module {
public:
  ProtobufModule(const std::string &name, const std::string &subspace_socket)
      : Module(name, subspace_socket) {}

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ProtobufSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel, const SubscriberOptions &options,
      std::function<void(const ProtobufSubscriber<MessageType> &,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSerializingSubscriber(channel, options, callback);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ProtobufSubscriber<MessageType>>>
  RegisterSubscriber(
      const std::string &channel,
      std::function<void(const ProtobufSubscriber<MessageType> &,
                         Message<const MessageType>, co::Coroutine *)>
          callback) {
    return RegisterSubscriber(channel, {}, std::move(callback));
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ProtobufPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots,
                    const PublisherOptions &options,
                    std::function<bool(const ProtobufPublisher<MessageType> &,
                                       MessageType &, co::Coroutine *)>
                        callback) {
    return RegisterSerializingPublisher(channel, slot_size, num_slots, options,
                                        callback);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ProtobufPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots,
                    std::function<bool(const ProtobufPublisher<MessageType> &,
                                       MessageType &, co::Coroutine *)>
                        callback) {
    return RegisterPublisher(channel, slot_size, num_slots, {}, callback);
  }

  template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ProtobufPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots,
                    const PublisherOptions &options) {
    return RegisterSerializingPublisher<MessageType,
                                        ProtobufSerializedLength<MessageType>,
                                        ProtobufSerialize<MessageType>>(
        channel, slot_size, num_slots, options);
  }

   template <typename MessageType>
  absl::StatusOr<std::shared_ptr<ProtobufPublisher<MessageType>>>
  RegisterPublisher(const std::string &channel, int slot_size, int num_slots) {
    return RegisterSerializingPublisher<MessageType,
                                        ProtobufSerializedLength<MessageType>,
                                        ProtobufSerialize<MessageType>>(
        channel, slot_size, num_slots);
  }
};
} // namespace stagezero::module
