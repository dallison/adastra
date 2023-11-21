#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "testdata/proto/chat.pb.h"

template <typename T> using Publisher = stagezero::module::ProtobufPublisher<T>;

template <typename T>
using Subscriber = stagezero::module::ProtobufSubscriber<T>;

template <typename T> using Message = stagezero::module::Message<T>;

class Listener : public stagezero::module::ProtobufModule {
public:
  Listener(const std::string &name, const std::string &subspace_server)
      : ProtobufModule(name, subspace_server) {}

  absl::Status Init(int argc, char **argv) override {
    auto pub = RegisterPublisher<chat::Answer>("chat", 256, 10);

    auto sub =
        RegisterSubscriber<chat::Question>("chat", [&pub](const Subscriber<chat::Question> &sub,
                                          Message<const chat::Question> msg,
                                          co::Coroutine* c) {
          std::cout << msg->serial_number() << " question: " << msg->text()
                    << std::endl;
          chat::Answer ans;
          ans.set_serial_number(msg->serial_number());
          ans.set_text(absl::StrFormat("The answer is %s", msg->text()));
          (*pub)->Publish(ans, c);
        });

    return absl::OkStatus();
  }
};

DEFINE_MODULE(Listener);
