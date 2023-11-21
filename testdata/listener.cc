#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "testdata/proto/chat.pb.h"

template <typename T> using Publisher = stagezero::module::ProtobufPublisher<T>;

template <typename T>
using Subscriber = stagezero::module::ProtobufSubscriber<T>;

template <typename T> using Message = stagezero::module::Message<T>;

class Listener : public stagezero::module::ProtobufModule {
public:
  Listener(const std::string &name, const std::string &subspace_socket)
      : ProtobufModule(name, subspace_socket) {}

  absl::Status Init(int argc, char **argv) override {
    auto pub = RegisterPublisher<chat::Answer>("answer", 256, 10);
    if (!pub.ok()) {
      return pub.status();
    }
    pub_ = std::move(*pub);

    auto sub = RegisterSubscriber<chat::Question>(
        "question",
        [this](const Subscriber<chat::Question> &sub,
               Message<const chat::Question> msg, co::Coroutine *c) {
          std::cout << msg->text() << std::endl;
          chat::Answer ans;
          ans.set_text(absl::StrFormat("%d", msg->x() * msg->y()));
          pub_->Publish(ans, c);
        });
    if (!sub.ok()) {
      return sub.status();
    }
    sub_ = std::move(*sub);
    return absl::OkStatus();
  }

private:
  std::shared_ptr<Publisher<chat::Answer>> pub_;
  std::shared_ptr<Subscriber<chat::Question>> sub_;
};

DEFINE_MODULE(Listener);
