#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "robot/proto/vision.pb.h"

template <typename T>
using Publisher = stagezero::module::ProtobufPublisher<T>;

template <typename T>
using Subscriber = stagezero::module::ProtobufSubscriber<T>;

template <typename T>
using Message = stagezero::module::Message<T>;

class Camera : public stagezero::module::ProtobufModule {
 public:
  Camera(stagezero::SymbolTable symbols)
      : ProtobufModule(std::move(symbols)) {}

  absl::Status Init(int argc, char **argv) override {
    stagezero::Symbol* name = symbols_.FindSymbol("camera_name");
    if (name == nullptr) {
      std::cerr << "No camera name supplied\n";
      abort();      
    }
    std::string channel_name = absl::StrFormat("/camera_%s", name->Value());
    auto pub = RegisterPublisher<robot::CameraImage>(
        channel_name, 1024*1024*4, 100,
        [](const Publisher<robot::CameraImage> &pub, robot::CameraImage &msg,
               co::Coroutine *c) -> bool {
          msg.set_rows(1024);
          msg.set_cols(1024);
          std::string image;
          for (int i = 0; i < 1024*1024*4; i++) {
            image += static_cast<char>(/*rand() & */0xff);
          }
          msg.set_image(image);
          return true;
        });
    if (!pub.ok()) {
      return pub.status();
    }
    pub_ = std::move(*pub);

    RunPeriodically(10, [this](co::Coroutine *c) { pub_->Publish(); });
    return absl::OkStatus();
  }

 private:
  std::shared_ptr<Publisher<robot::CameraImage>> pub_;
};

DEFINE_MODULE(Camera);
