#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "robot/proto/vision.pb.h"
#include "toolbelt/clock.h"

template <typename T> using Publisher = stagezero::module::ProtobufPublisher<T>;

template <typename T>
using Subscriber = stagezero::module::ProtobufSubscriber<T>;

template <typename T> using Message = stagezero::module::Message<T>;

class Stereo : public stagezero::module::ProtobufModule {
public:
  Stereo(stagezero::SymbolTable symbols) : ProtobufModule(std::move(symbols)) {
    // std::cout << "lldb -p " << getpid() << std::endl;
    // sleep(20);
  }

  absl::Status Init(int argc, char **argv) override {
    auto stereo = RegisterPublisher<robot::StereoImage>(
        "/stereo", 1024 * 1024 * 4 * 3, 16,
        [this](const Publisher<robot::StereoImage> &pub,
               robot::StereoImage &msg, co::Coroutine *c) -> bool {
          msg.mutable_header()->set_timestamp(toolbelt::Now());
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
        [this](const Subscriber<robot::CameraImage> &sub,
               Message<const robot::CameraImage> msg,
               co::Coroutine *c) { IncomingLeftCameraImage(msg); });
    if (!left_camera.ok()) {
      return left_camera.status();
    }
    left_camera_ = std::move(*left_camera);

    auto right_camera = RegisterSubscriber<robot::CameraImage>(
        "/camera_right",
        [this](const Subscriber<robot::CameraImage> &sub,
               Message<const robot::CameraImage> msg,
               co::Coroutine *c) { IncomingRightCameraImage(msg); });
    if (!right_camera.ok()) {
      return right_camera.status();
    }
    right_camera_ = std::move(*right_camera);

    return absl::OkStatus();
  }

private:
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
    return left - right; // Or something like this, only correct.
  }

  std::shared_ptr<Publisher<robot::StereoImage>> stereo_;
  std::shared_ptr<Subscriber<robot::CameraImage>> left_camera_;
  std::shared_ptr<Subscriber<robot::CameraImage>> right_camera_;

  Message<const robot::CameraImage> left_image_;
  Message<const robot::CameraImage> right_image_;
};

DEFINE_MODULE(Stereo);