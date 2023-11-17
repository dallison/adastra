#include "module/module.h"

class TestModule : public stagezero::module::Module {
public:
  TestModule(const std::string& name, const std::string& subspace_server) : Module(name, subspace_server) {}
 
  absl::Status Init(int argc, char** argv) override {
    int count = 0;
    RunPeriodically(2, [&count](co::Coroutine* c) {
      std::cerr << "tick " << count++ << std::endl;
    });
    return absl::OkStatus();
  }

};

DEFINE_MODULE(TestModule);
