#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "testdata/proto/chat.pb.h"

template <typename T> using Publisher = stagezero::module::ProtobufPublisher<T>;

template <typename T>
using Subscriber = stagezero::module::ProtobufSubscriber<T>;

template <typename T> using Message = stagezero::module::Message<T>;

class Talker : public stagezero::module::ProtobufModule {
public:
  Talker(const std::string &name, const std::string &subspace_server)
      : ProtobufModule(name, subspace_server) {}

  absl::Status Init(int argc, char **argv) override {
    int count = 0;

    auto pub = RegisterPublisher<chat::Question>(
        "chat", 256, 10,
        [&count](const Publisher<chat::Question> &pub, chat::Question &msg,
                 co::Coroutine *c) -> bool {
          msg.set_serial_number(++count);
          msg.set_text(absl::StrFormat("Question #%d", count));
          return true;
        });

    auto sub = RegisterSubscriber<chat::Answer>("chat", [](const Subscriber<chat::Answer> &sub,
                                             Message<const chat::Answer> msg, co::Coroutine* c) {
      std::cout << msg->serial_number() << " answer: " << msg->text()
                << std::endl;
    });

    RunPeriodically(2, [&pub](co::Coroutine *c) { (*pub)->Publish(); });
    return absl::OkStatus();
  }
};

DEFINE_MODULE(Talker);
