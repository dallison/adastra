#pragma once

#include "stagezero/client/client.h"
#include "toolbelt/logging.h"
#include <memory>

namespace adastra::capcom {

struct Compute;

// An umbilical connects capcom to StageZero via a client connection.
enum class UmbilicalState {
  kClosed,
  kConnecting,
  kConnected,
};

class Umbilical {
public:
  Umbilical(std::string name, toolbelt::Logger &logger,
            std::shared_ptr<Compute> compute,
            std::shared_ptr<stagezero::Client> client, bool is_static = false)
      : name_(name), logger_(logger), compute_(compute), client_(client),
        is_static_(is_static) {}

  ~Umbilical() { client_->Close(); }

  bool Precondition();

  absl::Status Connect(int event_mask, co::Coroutine *c);
  void Disconnect(bool dynamic_only = false);

  int IncStaticRefs(int inc) { return staticRefs_ += inc; }
  bool HasStaticRefs() const { return staticRefs_ > 0; }

  std::shared_ptr<Compute> GetCompute() const { return compute_; }
  std::shared_ptr<stagezero::Client> GetClient() const { return client_; }

  bool IsConnected() const {
    return client_ != nullptr && client_->IsConnected() &&
           state_ == UmbilicalState::kConnected;
  }

  bool IsStatic() const { return is_static_; }
  
private:
  std::string name_;
  toolbelt::Logger &logger_;

  std::shared_ptr<Compute> compute_ = nullptr;
  std::shared_ptr<stagezero::Client> client_{};
  UmbilicalState state_ = UmbilicalState::kClosed;
  int staticRefs_ = 0;
  int dynamicRefs_ = 0;
  bool is_static_ = false;
};

} // namespace adastra::capcom
