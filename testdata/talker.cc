#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "testdata/proto/chat.pb.h"

template <typename T>
using Publisher = stagezero::module::ProtobufPublisher<T>;

template <typename T>
using Subscriber = stagezero::module::ProtobufSubscriber<T>;

template <typename T>
using Message = stagezero::module::Message<T>;

class Talker : public stagezero::module::ProtobufModule {
 public:
  Talker(const std::string &name, const std::string &subspace_server)
      : ProtobufModule(name, subspace_server) {}

  absl::Status Init(int argc, char **argv) override {
    auto pub = RegisterPublisher<chat::Question>(
        "question", 256, 10,
        [this](const Publisher<chat::Question> &pub, chat::Question &msg,
               co::Coroutine *c) -> bool {
          msg.set_x(++count_);
          msg.set_y(3);
          msg.set_text(absl::StrFormat("What is %d times %d", count_, 3));
          return true;
        });
    if (!pub.ok()) {
      return pub.status();
    }
    pub_ = std::move(*pub);

    auto sub = RegisterSubscriber<chat::Answer>(
        "answer", [](const Subscriber<chat::Answer> &sub,
                     Message<const chat::Answer> msg, co::Coroutine *c) {
          std::cout << " The answer is " << msg->text() << std::endl;
        });

    if (!sub.ok()) {
      return sub.status();
    }
    sub_ = std::move(*sub);

    RunPeriodically(2, [this](co::Coroutine *c) { pub_->Publish(); });
    return absl::OkStatus();
  }

 private:
  int count_ = 0;
  std::shared_ptr<Publisher<chat::Question>> pub_;
  std::shared_ptr<Subscriber<chat::Answer>> sub_;
};

DEFINE_MODULE(Talker);
