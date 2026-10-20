#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "robot/proto/vision.phaser.h"
#include "toolbelt/clock.h"

// Zero copy protobuf messages using Phaser.  This allows
// you to use a protobuf message whose storage is in a
// Subspace shared memory channel and does not require
// serialization.  The subscribers must also be configured to receive
// Phaser messages as the wireformat is different from protobuf.
template <typename T> using Publisher = adastra::module::PhaserPublisher<T>;

template <typename T> using Subscriber = adastra::module::PhaserSubscriber<T>;

template <typename T> using Message = adastra::module::Message<T>;
template <typename T> using WeakMessage = adastra::module::WeakMessage<T>;

class Stereo : public adastra::module::ProtobufModule {
public:
  explicit Stereo(std::unique_ptr<adastra::stagezero::SymbolTable> symbols)
      : Module(std::move(symbols)) {}

  // A stereo image consists of 2 camera images and a disparity
  // image, each of which is 256X256 pixels (RGB).
  static constexpr uint64_t kMaxMessageSize = 3 * 256 * 256 * 3 + 1000;
  static constexpr int32_t kNumSlots = 16;

  absl::Status Init(int argc, char **argv) override {
    auto stereo = RegisterPublisher<robot::phaser::StereoImage>(
        "/stereo", kMaxMessageSize, kNumSlots,
        [this](auto pub, auto &msg, auto c) -> size_t {
          msg.mutable_header()->set_timestamp(toolbelt::Now());
          // Even though we are zero-copy for this message, we need to copy
          // the camera images into the stereo image. They are held as weak
          // messages and might have expired, which is fine.
          //
          // Another way to do this is to hold the actual camera images in
          // shared memory and send a reference to them instead of copying.
          // I've done that before and it was very successful.
          if (!left_image_.expired()) {
            auto left = left_image_.lock();
            msg.mutable_left()->CopyFrom(*left);
          }
          if (!left_image_.expired()) {
            auto right = right_image_.lock();
            msg.mutable_right()->CopyFrom(*right);
          }

          CalculateDisparity(msg);
          // Reset the stored images so we don't send them twice.
          left_image_.reset();
          right_image_.reset();
          // std::cout << "Stereo image published with timestamp " << msg.header().timestamp() << std::endl;
          return msg.ByteSizeLong();
        });
    stereo_ = std::move(*stereo);

    auto left_camera = RegisterSubscriber<robot::phaser::CameraImage>(
        "/camera_left", {.max_shared_ptrs = 3},
        [this](auto sub, auto msg, auto c) { IncomingLeftCameraImage(msg); });
    if (!left_camera.ok()) {
      return left_camera.status();
    }

    auto right_camera = RegisterSubscriber<robot::phaser::CameraImage>(
        "/camera_right", {.max_shared_ptrs = 3},
        [this](auto sub, auto msg, auto c) { IncomingRightCameraImage(msg); });
    if (!right_camera.ok()) {
      return right_camera.status();
    }

    return absl::OkStatus();
  }

private:
  // In a real robot we would try to synchronize the camera images
  // by keeping a couple of them and looking for two images with
  // similar timestamps.  That's outside the scope of this demonstration,
  // so we just send a stereo image when we have an image from each
  // camera.
  void
  IncomingLeftCameraImage(Message<const robot::phaser::CameraImage> image) {
    left_image_ = image;
    if (!right_image_.expired()) {
      stereo_->Publish();
    }
  }

  void
  IncomingRightCameraImage(Message<const robot::phaser::CameraImage> image) {
    right_image_ = image;
    if (!left_image_.expired()) {
     stereo_->Publish();
    }
  }

  void CalculateDisparity(robot::phaser::StereoImage &msg) {
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

  std::shared_ptr<Publisher<robot::phaser::StereoImage>> stereo_;

  // We store weak messages here.  For this test it really doesn't matter
  // but if we were storing a history buffer we don't want to lock the
  // IPC slots for all the messages.  This serves as an example of how
  // to use weak pointers.
  WeakMessage<const robot::phaser::CameraImage> left_image_;
  WeakMessage<const robot::phaser::CameraImage> right_image_;
};

DEFINE_MODULE(Stereo);