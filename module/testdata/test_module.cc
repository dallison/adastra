#include "module/protobuf_module.h"

class TestModule : public adastra::module::ProtobufModule {
 public:
  TestModule(std::unique_ptr<adastra::stagezero::SymbolTable> symbols)
      : ProtobufModule(std::move(symbols)) {}

  absl::Status Init(int argc, char** argv) override {
    int count = 0;
    RunPeriodically(2, [&count](co::Coroutine* c) {
      std::cerr << "tick " << count++ << std::endl;
    });
    return absl::OkStatus();
  }
};

DEFINE_MODULE(TestModule);
