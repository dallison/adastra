#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "robot/proto/vision.pb.h"
#include "toolbelt/clock.h"

template <typename T> using Publisher = adastra::module::ProtobufPublisher<T>;

template <typename T>
using Subscriber = adastra::module::ProtobufSubscriber<T>;

template <typename T> using Message = adastra::module::Message<T>;

class Camera : public adastra::module::ProtobufModule {
public:
  Camera(std::unique_ptr<adastra::stagezero::SymbolTable> symbols) : ProtobufModule(std::move(symbols)) {}

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
    constexpr uint64_t kMaxMessageSize = 256 * 256 * 3 + 32;
    constexpr int32_t kNumSlots = 16;

    std::string channel_name = absl::StrFormat("/camera_%s", name->Value());
    auto pub = RegisterPublisher<robot::CameraImage>(
        channel_name, kMaxMessageSize, kNumSlots,
        [](auto pub, auto &msg, auto c) -> bool {
          msg.mutable_header()->set_timestamp(toolbelt::Now());
          msg.set_rows(1024);
          msg.set_cols(1024);
          std::string image;
          for (int i = 0; i < 256 * 256 * 3; i++) {
            image += static_cast<char>(rand() & 0xff);
          }
          msg.set_image(image);
          return true;
        });
    if (!pub.ok()) {
      return pub.status();
    }

    // Send the camera images at 10Hz.
    RunPeriodically(10, [pub = *pub](co::Coroutine *c) { pub->Publish(); });
    return absl::OkStatus();
  }
};

DEFINE_MODULE(Camera);
