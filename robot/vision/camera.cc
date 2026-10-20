#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "robot/proto/vision.phaser.h"
#include "toolbelt/clock.h"

// We publish zero-copy protobuf messages as the images are large.
// Phaser is a zero-copy protobuf compiler back-end.
// This allows you to use a protobuf message whose storage is in a
// Subspace shared memory channel and does not require
// serialization.  The subscribers must also be configured to receive
// Phaser messages as the wireformat is different from protobuf.
template <typename T> using Publisher = adastra::module::PhaserPublisher<T>;

template <typename T> using Subscriber = adastra::module::PhaserSubscriber<T>;

template <typename T> using Message = adastra::module::Message<T>;

class Camera : public adastra::module::ProtobufModule {
public:
  explicit Camera(std::unique_ptr<adastra::stagezero::SymbolTable> symbols)
      : Module(std::move(symbols)) {}

  absl::Status Init(int argc, char **argv) override {
    adastra::stagezero::Symbol *name = symbols_->FindSymbol("camera_name");
    if (name == nullptr) {
      std::cerr << "No camera name supplied\n";
      abort();
    }
    // A camera image is 256X256 pixels, each of which is 3 bytes (RGB).
    // We also need some overhead for the header, rows and columns fields.
    // In reality the camera images would be bigger, but this is just
    // a demo.
    constexpr uint64_t kImageSize = 256 * 256 * 3;
    constexpr uint64_t kMaxMessageSize = kImageSize + 1000;
    constexpr int32_t kNumSlots = 16;

    std::string channel_name = absl::StrFormat("/camera_%s", name->Value());
    auto pub = RegisterPublisher<robot::phaser::CameraImage>(
        channel_name, kMaxMessageSize, kNumSlots,
        [channel_name](auto pub, auto &msg, auto c) -> size_t {
          msg.mutable_header()->set_timestamp(toolbelt::Now());
          msg.set_rows(1024);
          msg.set_cols(1024);
          // Direct access to image because of zero-copy.
          absl::Span<char> image = msg.allocate_image(kImageSize);
          for (int i = 0; i < kImageSize; i++) {
            image[i] = static_cast<char>(rand() & 0xff);
          }
          return msg.ByteSizeLong();
        });
    if (!pub.ok()) {
      return pub.status();
    }

    // Send the camera images at 10Hz.
    RunPeriodically(10, [ pub = *pub ](co::Coroutine * c) {
      pub->Publish();
    });
    return absl::OkStatus();
  }
};

DEFINE_MODULE(Camera);
