#pragma once

#include "absl/container/flat_hash_map.h"
#include "toolbelt/hexdump.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "toolbelt/triggerfd.h"
#include <list>
#include <memory>

#include "coroutine.h"

namespace stagezero::common {

template <typename Request, typename Response, typename Event>
class TCPClientHandler : public std::enable_shared_from_this<
                             TCPClientHandler<Request, Response, Event>> {
public:
  TCPClientHandler(toolbelt::TCPSocket socket)
      : command_socket_(std::move(socket)) {}
  virtual ~TCPClientHandler() = default;

  void Run(co::Coroutine *c);

  const std::string &GetClientName() const { return client_name_; }

  virtual toolbelt::Logger &GetLogger() const = 0;

  virtual co::CoroutineScheduler &GetScheduler() const = 0;

  virtual void AddCoroutine(std::unique_ptr<co::Coroutine> c) = 0;

  char *GetEventBuffer() { return event_buffer_; }

  toolbelt::TCPSocket &GetEventSocket() { return event_socket_; }
  void SetEventSocket(toolbelt::TCPSocket socket) {
    event_socket_ = std::move(socket);
  }

protected:
  void EventSenderCoroutine(co::Coroutine *c);
  absl::StatusOr<int> Init(const std::string &client_name, co::Coroutine *c);

  static constexpr size_t kMaxMessageSize = 4096;

  virtual absl::Status HandleMessage(const Request &req, Response &resp,
                                     co::Coroutine *c) = 0;

  absl::Status QueueEvent(std::unique_ptr<Event> event);

  toolbelt::TCPSocket command_socket_;
  char command_buffer_[kMaxMessageSize];
  std::string client_name_ = "unknown";

  char event_buffer_[kMaxMessageSize];
  toolbelt::TCPSocket event_socket_;
  std::unique_ptr<co::Coroutine> event_channel_acceptor_;

  std::list<std::unique_ptr<Event>> events_;
  toolbelt::TriggerFd event_trigger_;
};

template <typename Request, typename Response, typename Event>
inline void TCPClientHandler<Request, Response, Event>::Run(co::Coroutine *c) {
  // The data is placed 4 bytes into the buffer.  The first 4
  // bytes of the buffer are used by SendMessage and ReceiveMessage
  // for the length of the data.
  char *sendbuf = command_buffer_ + sizeof(int32_t);
  constexpr size_t kSendBufLen = sizeof(command_buffer_) - sizeof(int32_t);
  for (;;) {
    absl::StatusOr<ssize_t> n = command_socket_.ReceiveMessage(
        command_buffer_, sizeof(command_buffer_), c);
    if (!n.ok()) {
      printf("ReceiveMessage error %s\n", n.status().ToString().c_str());
      return;
    }
    std::cout << getpid() << " " << this << " server received\n";
    toolbelt::Hexdump(command_buffer_, *n);

    Request request;
    if (request.ParseFromArray(command_buffer_, *n)) {
      Response response;
      if (absl::Status s = HandleMessage(request, response, c); !s.ok()) {
        GetLogger().Log(toolbelt::LogLevel::kError, "%s\n",
                        s.ToString().c_str());
        return;
      }

      std::cout << getpid() << " server sending " << response.DebugString()
                << std::endl;
      if (!response.SerializeToArray(sendbuf, kSendBufLen)) {
        GetLogger().Log(toolbelt::LogLevel::kError,
                        "Failed to serialize response\n");
        return;
      }
      size_t msglen = response.ByteSizeLong();
      std::cout << getpid() << " SERVER SEND\n";
      toolbelt::Hexdump(sendbuf, msglen);
      absl::StatusOr<ssize_t> n =
          command_socket_.SendMessage(sendbuf, msglen, c);
      if (!n.ok()) {
        return;
      }
    } else {
      GetLogger().Log(toolbelt::LogLevel::kError, "Failed to parse message\n");
      return;
    }
  }
}

template <typename Request, typename Response, typename Event>
inline absl::StatusOr<int>
TCPClientHandler<Request, Response, Event>::Init(const std::string &client_name,
                                                 co::Coroutine *c) {
  client_name_ = client_name;

  // Event channel is an ephemeral port.
  toolbelt::InetAddress event_channel_addr = command_socket_.BoundAddress();
  event_channel_addr.SetPort(0);

  std::cout << "binding event channel to " << event_channel_addr.ToString()
            << std::endl;
  // Open listen socket.
  toolbelt::TCPSocket listen_socket;
  if (absl::Status status = listen_socket.SetCloseOnExec(); !status.ok()) {
    return status;
  }

  if (absl::Status status = listen_socket.Bind(event_channel_addr, true);
      !status.ok()) {
    return status;
  }
  int event_port = listen_socket.BoundAddress().Port();

  if (absl::Status status = event_trigger_.Open(); !status.ok()) {
    return status;
  }

  // Spawn a coroutine to accept the event channel connection.
  event_channel_acceptor_ =
      std::make_unique<co::Coroutine>(GetScheduler(), [
        client = this->shared_from_this(), listen_socket = std::move(listen_socket)
      ](co::Coroutine * c2) mutable {
        std::cout << "accepting event channel " << std::endl;
        absl::StatusOr socket = listen_socket.Accept(c2);
        if (!socket.ok()) {
          client->GetLogger().Log(toolbelt::LogLevel::kError,
                                  "Failed to open event channel: %s",
                                  socket.status().ToString().c_str());
          return;
        }

        if (absl::Status status = socket->SetCloseOnExec(); !status.ok()) {
          client->GetLogger().Log(
              toolbelt::LogLevel::kError,
              "Failed to set close-on-exec on event channel: %s",
              socket.status().ToString().c_str());
          return;
        }
        client->SetEventSocket(std::move(*socket));
        client->GetLogger().Log(toolbelt::LogLevel::kInfo,
                                "Event channel open");

        // Start the event sender coroutine.
        client->AddCoroutine(std::make_unique<co::Coroutine>(
            client->GetScheduler(),
            [client](co::Coroutine *c2) { client->EventSenderCoroutine(c2); }));
      });
  return event_port;
}

template <typename Request, typename Response, typename Event>
inline absl::Status TCPClientHandler<Request, Response, Event>::QueueEvent(
    std::unique_ptr<Event> event) {
  if (!event_socket_.Connected()) {
    return absl::InternalError(
        "Unable to send event: event socket is not connected");
  }
  std::cerr << "queueing event" << std::endl;
  events_.push_back(std::move(event));
  event_trigger_.Trigger();
  return absl::OkStatus();
}

template <typename Request, typename Response, typename Event>
inline void TCPClientHandler<Request, Response, Event>::EventSenderCoroutine(
    co::Coroutine *c) {
  auto client = this->shared_from_this();
  while (client->event_socket_.Connected()) {
    std::cerr << "Waiting for event to be queued" << std::endl;
    // Wait for an event to be queued.
    c->Wait(client->event_trigger_.GetPollFd().Fd(), POLLIN);
    client->event_trigger_.Clear();

    // Empty event queue by sending all events before going back
    // to waiting for the trigger.  This ensures that we never have
    // something in the event queue after the trigger has been
    // cleared.
    while (!client->events_.empty()) {
      std::cerr << "Got queued event" << std::endl;

      // Remove event from the queue and send it.
      std::unique_ptr<Event> event = std::move(client->events_.front());
      client->events_.pop_front();
      std::cerr << event->DebugString() << std::endl;

      char *sendbuf = client->event_buffer_ + sizeof(int32_t);
      constexpr size_t kSendBufLen =
          sizeof(client->event_buffer_) - sizeof(int32_t);
      if (!event->SerializeToArray(sendbuf, kSendBufLen)) {
        client->GetLogger().Log(toolbelt::LogLevel::kError,
                                "Failed to serialize event");
      } else {
        size_t msglen = event->ByteSizeLong();
        absl::StatusOr<ssize_t> n =
            client->event_socket_.SendMessage(sendbuf, msglen, c);
        if (!n.ok()) {
          client->GetLogger().Log(toolbelt::LogLevel::kError,
                                  "Failed to serialize event",
                                  n.status().ToString().c_str());
        }
      }
    }
  }
}
} // namespace stagezero::common
