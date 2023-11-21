#include "module/protobuf_module.h"

class TestModule : public stagezero::module::ProtobufModule {
public:
  TestModule(const std::string& name, const std::string& subspace_server) : ProtobufModule(name, subspace_server) {}
 
  absl::Status Init(int argc, char** argv) override {
    int count = 0;
    RunPeriodically(2, [&count](co::Coroutine* c) {
      std::cerr << "tick " << count++ << std::endl;
    });
    return absl::OkStatus();
  }

};

DEFINE_MODULE(TestModule);
