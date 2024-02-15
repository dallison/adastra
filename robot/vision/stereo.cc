#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "robot/proto/vision.pb.h"
#include "toolbelt/clock.h"

template <typename T> using Publisher = adastra::module::ProtobufPublisher<T>;

template <typename T>
using Subscriber = adastra::module::ProtobufSubscriber<T>;

template <typename T> using Message = adastra::module::Message<T>;

class Stereo : public adastra::module::ProtobufModule {
public:
  explicit Stereo(std::unique_ptr<adastra::stagezero::SymbolTable> symbols)
      : ProtobufModule(std::move(symbols)) {}

  // A stereo image consists of 2 camera images and a disparity
  // image, each of which is 256X256 pixels (RGB).
  static constexpr uint64_t kMaxMessageSize = 3 * 256 * 256 * 3 + 32;
  static constexpr int32_t kNumSlots = 16;

  absl::Status Init(int argc, char **argv) override {
    auto stereo = RegisterPublisher<robot::StereoImage>(
        "/stereo", kMaxMessageSize, kNumSlots,
        [this](auto pub, auto &msg, auto c) -> bool {
          msg.mutable_header()->set_timestamp(toolbelt::Now());
          // It would be better to move the images into the stereo
          // image, but protobuf doesn't provide a way to do that
          // as far as I know.
          msg.mutable_left()->CopyFrom(*left_image_);
          msg.mutable_right()->CopyFrom(*right_image_);

          CalculateDisparity(msg);
          left_image_.reset();
          right_image_.reset();
          return true;
        });
    stereo_ = std::move(*stereo);

    auto left_camera = RegisterSubscriber<robot::CameraImage>(
        "/camera_left",
        [this](auto sub, auto msg, auto c) { IncomingLeftCameraImage(msg); });
    if (!left_camera.ok()) {
      return left_camera.status();
    }

    auto right_camera = RegisterSubscriber<robot::CameraImage>(
        "/camera_right",
        [this](auto sub, auto msg, auto c) { IncomingRightCameraImage(msg); });
    if (!right_camera.ok()) {
      return right_camera.status();
    }

    return absl::OkStatus();
  }

private:
  // In a real robot we would try to synchronize the camera images
  // by keeping a couple of them and looking for two images with
  // similar timestamps.  That's outside the scope of this demonostration,
  // so we just send a stereo image when we have an image from each
  // camera.
  void IncomingLeftCameraImage(Message<const robot::CameraImage> image) {
    if (left_image_ == nullptr) {
      left_image_ = image;
      if (right_image_ != nullptr) {
        stereo_->Publish();
      }
    }
  }

  void IncomingRightCameraImage(Message<const robot::CameraImage> image) {
    if (right_image_ == nullptr) {
      right_image_ = image;
      if (left_image_ != nullptr) {
        stereo_->Publish();
      }
    }
  }

  void CalculateDisparity(robot::StereoImage &msg) {
    auto *disparity = msg.mutable_disparity();
    int num_rows = msg.left().rows();
    int num_cols = msg.left().cols();
    disparity->set_rows(num_rows);
    disparity->set_cols(num_cols);
    std::string disparity_image;
    for (int row = 0; row < num_rows; row++) {
      for (int col = 0; col < num_cols; col++) {
        int index = row * num_rows + col;
        disparity_image +=
            Disparity(msg.left().image()[index], msg.right().image()[index]);
      }
    }
    disparity->set_image(std::move(disparity_image));
  }

  char Disparity(char left, char right) {
    return left - right; // Or something like this.
  }

  std::shared_ptr<Publisher<robot::StereoImage>> stereo_;

  Message<const robot::CameraImage> left_image_;
  Message<const robot::CameraImage> right_image_;
};

DEFINE_MODULE(Stereo);