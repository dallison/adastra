#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "absl/types/span.h"
#include "module/protobuf_module.h"
#include "module/ros_module.h"
#include "robot/proto/localizer.pb.h"
#include "robot/proto/map.pb.h"
#include "robot/proto/vision.phaser.h"
#include "robot/ros_msgs/zeros/robot_msgs/GpsLocation.h"
#include "toolbelt/clock.h"

template <typename T>
using ProtobufSubscriber = adastra::module::ProtobufSubscriber<T>;
template <typename T>
using PhaserSubscriber = adastra::module::PhaserSubscriber<T>;

template <typename T>
using ProtobufPublisher = adastra::module::ProtobufPublisher<T>;
template <typename T>
using PhaserPublisher = adastra::module::PhaserPublisher<T>;

template <typename T> using Message = adastra::module::Message<T>;

static inline constexpr char kMapRequestChannel[] = "/map_request";
static inline constexpr char kMapResponseChannel[] = "/map_response";
static inline constexpr char kLocalizerStatus[] = "/localizer_status";
static inline constexpr char kStereo[] = "/stereo";
static inline constexpr char kGps[] = "/gps";

static constexpr uint64_t kMaxMessageSize = 8192;
static constexpr int32_t kNumSlots = 16;

class Localizer : public adastra::module::ProtobufModule,
                  public adastra::module::ROSModule {
public:
  explicit Localizer(std::unique_ptr<adastra::stagezero::SymbolTable> symbols)
      : Module(std::move(symbols)) {}

  absl::Status Init(int argc, char **argv) override {
    auto status_publisher =
        ProtobufModule::RegisterPublisher<robot::LocalizationStatus>(
            kLocalizerStatus, kMaxMessageSize, kNumSlots);
    if (!status_publisher.ok()) {
      return status_publisher.status();
    }
    status_publisher_ = std::move(*status_publisher);

    // This is zero-copy protobuf.
    auto stereo_image_subscriber =
        ProtobufModule::RegisterSubscriber<robot::phaser::StereoImage>(
            kStereo, {.max_shared_ptrs = 3},
            [this](auto sub, auto msg, auto c) {
              IncomingStereoImage(msg, c);
            } );
    if (!stereo_image_subscriber.ok()) {
      return stereo_image_subscriber.status();
    }

    // This is zero copy ROS.
    auto gps_subscriber =
        ROSModule::RegisterSubscriber<robot_msgs::zeros::GpsLocation>(
            kGps, [this](auto sub, auto msg, auto c) {
              IncomingGpsLocation(msg, c);
            });
    if (!gps_subscriber.ok()) {
      return gps_subscriber.status();
    }

    RunNow([this](co::Coroutine *c) {
      if (absl::Status status = LoadMap(c); !status.ok()) {
        std::cerr << "Failed to load map: " << status << std::endl;
      }
    });

    return absl::OkStatus();
  }

private:
  void IncomingStereoImage(Message<const robot::phaser::StereoImage> image,
                           co::Coroutine *c) {
    std::cout << "Stereo image received with timestamp " << image->header().timestamp() << "\n";
  }

  void IncomingGpsLocation(Message<const robot_msgs::zeros::GpsLocation> loc,
                           co::Coroutine *c) {

    std::cout << "GPS location: " << double(loc->latitude) << " "
              << double(loc->longitude) << std::endl;
  }

  absl::Status LoadMap(co::Coroutine *c) {
    auto open_req = ProtobufModule::RegisterPublisher<robot::MapRequest>(
        kMapRequestChannel, 32, 16, {.reliable = true},
        [](auto pub, auto &msg, auto c) -> size_t {
          std::cout << "opening map server\n";
          auto open = msg.mutable_open();
          open->set_client_name("localizer");
          msg.mutable_header()->set_timestamp(toolbelt::Now());
          msg.set_request_id(1);
          return msg.ByteSizeLong();
        });
    if (!open_req.ok()) {
      return open_req.status();
    }

    auto open_resp = ProtobufModule::RegisterSubscriber<robot::MapResponse>(
        kMapResponseChannel,
        [ this, open_req = *open_req ](auto sub, auto msg, auto c) {
          // We don't need the open request publisher or subscriber now.
          RemovePublisher(open_req);
          RemoveSubscriber(sub);

          if (msg->resp_case() == robot::MapResponse::kOpen) {
            if (!msg->open().error().empty()) {
              std::cerr << msg->open().error() << std::endl;
              return;
            }
            std::string request_channel = msg->open().request_channel();
            std::string response_channel = msg->open().response_channel();

            auto load_req =
                ProtobufModule::RegisterPublisher<robot::MapRequest>(
                    request_channel, 32, 16, {.reliable = true},
                    [](auto pub, auto &msg, auto c) -> size_t {
                      std::cout << "loading map\n";
                      auto load = msg.mutable_load();
                      msg.mutable_header()->set_timestamp(toolbelt::Now());
                      msg.set_request_id(1);
                      load->set_region_id(1234);
                      load->set_x(10);
                      load->set_y(20);
                      load->set_width(100);
                      load->set_height(100);
                      return msg.ByteSizeLong();
                    });
            if (!load_req.ok()) {
              std::cerr << "Failed to open map request: " << load_req.status()
                        << std::endl;
              return;
            }

            auto load_resp =
                ProtobufModule::RegisterSubscriber<robot::MapResponse>(
                    response_channel,
                    [ this, load_req = *load_req ](auto sub, auto msg, auto c) {
                      IncomingMapTile(load_req, sub, msg);
                    });
            if (!load_resp.ok()) {
              std::cerr << "Failed to load map: " << load_resp.status()
                        << std::endl;
              return;
            }

            // Send load request.
            (*load_req)->Publish();
          } else {
            std::cerr << "Unknown open response\n";
          }
        });
    if (!open_resp.ok()) {
      return open_resp.status();
    }
    (*open_req)->Publish();

    return absl::OkStatus();
  }

  void
  IncomingMapTile(std::shared_ptr<adastra::module::PublisherBase> load_req,
                  std::shared_ptr<adastra::module::SubscriberBase> load_resp,
                  Message<const robot::MapResponse> resp) {
    auto &load = resp->load();
    if (!load.error().empty()) {
      std::cerr << load.error() << std::endl;
      return;
    }
    std::cout << "got map tile: " << load.tile().id() << std::endl;
    if (load.last_tile()) {
      RemovePublisher(load_req);
      RemoveSubscriber(load_resp);
      std::cout << "got last tile\n";

      // Close the map request.
      auto close_req = ProtobufModule::RegisterPublisher<robot::MapRequest>(
          kMapRequestChannel, 32, 16, {.reliable = true},
          [](auto pub, auto &msg, auto c) -> size_t {
            std::cout << "closing connection\n";
            auto close = msg.mutable_close();
            close->set_client_name("localizer");
            msg.mutable_header()->set_timestamp(toolbelt::Now());
            return msg.ByteSizeLong();
          });
      if (!close_req.ok()) {
        std::cerr << "Failed to close map request: " << close_req.status()
                  << std::endl;
        return;
      }

      auto close_resp = ProtobufModule::RegisterSubscriber<robot::MapResponse>(
          kMapResponseChannel,
          [ this, close_req = *close_req ](auto sub, auto msg, auto c) {
            RemovePublisher(close_req);
            RemoveSubscriber(sub);
            if (!msg->close().error().empty()) {
              std::cerr << "Failed to close map request: "
                        << msg->close().error() << std::endl;
            } else {
              std::cout << "Map Server connection closed\n";
            }
          });
      if (!close_resp.ok()) {
        std::cerr << "Failed to close map: " << close_resp.status()
                  << std::endl;
        return;
      }

      // Send close request.
      (*close_req)->Publish();
    }
  }

  std::shared_ptr<ProtobufPublisher<robot::LocalizationStatus>>
      status_publisher_;
};

DEFINE_MODULE(Localizer);
