
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "absl/types/span.h"
#include "module/protobuf_module.h"
#include "robot/proto/map.pb.h"
#include "toolbelt/clock.h"

#include <list>

// This is an RPC-style module that distributes map segments
// upon request.  It uses reliable channels to make sure that
// the map segments are not lost in transit.  Maps can be very
// big and missing map segments are problematic for navigation
// and localization.

template <typename T>
using Subscriber = adastra::module::ProtobufSubscriber<T>;

template <typename T> using Publisher = adastra::module::ProtobufPublisher<T>;

template <typename T> using Message = adastra::module::Message<T>;

static inline constexpr char kMapRequestChannel[] = "/map_request";
static inline constexpr char kMapResponseChannel[] = "/map_response";

class MapServer : public adastra::module::ProtobufModule {
public:
  explicit MapServer(std::unique_ptr<adastra::stagezero::SymbolTable> symbols)
      : ProtobufModule(std::move(symbols)) {}

  absl::Status Init(int argc, char **argv) override {
    auto resp = RegisterPublisher<robot::MapResponse>(
        kMapResponseChannel, kMaxResponseMessageSize, kNumResponseSlots,
        {.reliable = true}, [this](auto pub, auto &msg, auto c) -> bool {
          if (pending_responses_.empty()) {
            return false;
          }
          auto resp = pending_responses_.front();
          pending_responses_.pop_front();
          msg = *resp;
          return true;
        });
    if (!resp.ok()) {
      return resp.status();
    }
    response_ = std::move(*resp);

    auto req = RegisterSubscriber<robot::MapRequest>(
        kMapRequestChannel, {.reliable = true},
        [this](auto sub, auto msg, auto c) { IncomingRequest(msg, c); });
    if (!req.ok()) {
      return req.status();
    }

    return absl::OkStatus();
  }

private:
  static constexpr uint64_t kMaxResponseMessageSize = 4096;
  static constexpr int32_t kNumResponseSlots = 16;

  struct MapTile {
    int id;
    off_t file_offset;
    size_t size;
  };

  struct ChannelHandler {
    ChannelHandler(int req_id, const std::string &req, const std::string &resp)
        : request_id(req_id), request_channel(req), response_channel(resp) {}
    int request_id;
    std::string request_channel;
    std::string response_channel;
    std::list<MapTile> tiles;
    std::string error;
    std::shared_ptr<adastra::module::PublisherBase> pub;
    std::shared_ptr<adastra::module::SubscriberBase> sub;
  };

  void IncomingRequest(Message<const robot::MapRequest> msg, co::Coroutine *c) {
    auto resp = std::make_shared<robot::MapResponse>();
    switch (msg->req_case()) {
    case robot::MapRequest::kOpen: {
      auto &open = msg->open();
      auto open_resp = resp->mutable_open();
      auto handler = NewChannel(open.client_name(), msg->request_id());
      if (!handler.ok()) {
        open_resp->set_error(handler.status().ToString());
        break;
      }
      open_resp->set_request_channel((*handler)->request_channel);
      open_resp->set_response_channel((*handler)->response_channel);
      channels_.insert(std::make_pair(open.client_name(), std::move(*handler)));
      break;
    }
    case robot::MapRequest::kClose: {
      auto &close = msg->close();
      auto close_resp = resp->mutable_close();
      if (absl::Status status = CloseChannel(close.client_name());
          !status.ok()) {
        close_resp->set_error(status.ToString());
      }
      break;
    }
    default:
      resp->set_error("Invalid map request");
      break;
    }
    resp->mutable_header()->set_timestamp(toolbelt::Now());
    pending_responses_.push_back(resp);
    response_->Publish();
  }

  absl::StatusOr<std::shared_ptr<ChannelHandler>>
  NewChannel(const std::string &client_name, int request_id) {
    std::string req_channel_name = absl::StrFormat("/%s/request", client_name);
    std::string resp_channel_name =
        absl::StrFormat("/%s/response", client_name);

    auto channel_handler = std::make_shared<ChannelHandler>(
        request_id, req_channel_name, resp_channel_name);

    auto resp = RegisterPublisher<robot::MapResponse>(
        resp_channel_name, kMaxResponseMessageSize, kNumResponseSlots,
        {.reliable = true},
        [this, channel_handler](auto pub, auto &msg, auto c) -> bool {
          SendTile(msg, channel_handler);
          return true;
        });

    if (!resp.ok()) {
      return resp.status();
    }

    channel_handler->pub = *resp;

    auto sub = RegisterSubscriber<robot::MapRequest>(
        req_channel_name, {.reliable = true},
        [ this, resp = std::move(*resp), channel_handler ](auto sub, auto msg,
                                                           auto c) {
          // Incoming request on the open channel.
          switch (msg->req_case()) {
          case robot::MapRequest::kLoad: {
            auto &load = msg->load();
            BuildMapTiles(load, channel_handler);
            // Send all tiles now.  The publisher callback will be called when
            // we can send the tile to the requestor.
            for (size_t i = 0; i < channel_handler->tiles.size(); i++) {
              resp->Publish();
            }
            break;
          }
          default:
            // Invalid request, send back a single response with the error.
            channel_handler->error = "Invalid map request";
            resp->Publish();
            break;
          }
        });
    if (!sub.ok()) {
      return sub.status();
    }
    channel_handler->sub = *sub;

    return channel_handler;
  }

  void SendTile(robot::MapResponse &msg,
                std::shared_ptr<ChannelHandler> channel_handler) {
    msg.set_response_id(channel_handler->request_id);
    auto *load_resp = msg.mutable_load();
    // If we have an error, send it back.
    if (!channel_handler->error.empty()) {
      load_resp->set_error(channel_handler->error);
      return;
    }

    // Send a single map tile.
    auto *tile = load_resp->mutable_tile();
    MapTile &t = channel_handler->tiles.front();
    tile->set_id(t.id);
    // Here we would load the data from the map file and put it into
    // the tile.  Since we don't actually have a map file, we just
    // fill it will some random bytes.  4000 seems a reasonable number.
    std::string data;
    for (int i = 0; i < 4000; i++) {
      data += static_cast<char>(rand() & 0xff);
    }
    tile->set_data(data);
    channel_handler->tiles.pop_front();
    if (channel_handler->tiles.empty()) {
      load_resp->set_last_tile(true);
    }
  }

  void BuildMapTiles(const robot::LoadMapSegmentRequest &req,
                     std::shared_ptr<ChannelHandler> handler) {
    // Build up a set of tiles to send back in the response.  They
    // will all be sent in sequence using a reliable channel, so no
    // data loss.
    for (int i = 0; i < 100; i++) {
      handler->tiles.push_back({.id = i});
      // And other things we need for a real map tile.
    }
  }

  absl::Status CloseChannel(const std::string &client_name) {
    auto it = channels_.find(client_name);
    if (it != channels_.end()) {
      RemovePublisher(it->second->pub);
      RemoveSubscriber(it->second->sub);
      channels_.erase(it);
      return absl::OkStatus();
    }
    return absl::InternalError(
        absl::StrFormat("No such client %s", client_name));
  }

  std::shared_ptr<Publisher<robot::MapResponse>> response_;
  absl::flat_hash_map<std::string, std::shared_ptr<ChannelHandler>> channels_;
  std::list<std::shared_ptr<robot::MapResponse>> pending_responses_;
};

DEFINE_MODULE(MapServer);
