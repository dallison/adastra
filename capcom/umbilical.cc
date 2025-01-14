#include "capcom/capcom.h"

namespace adastra::capcom {

bool Umbilical::Precondition() {
  bool was_closed = state_ == UmbilicalState::kClosed;
  state_ = UmbilicalState::kConnecting;
  dynamicRefs_++;
  return was_closed;
}

absl::Status Umbilical::Connect(int event_mask, co::Coroutine *c) {
  if (state_ != UmbilicalState::kConnecting) {
    return absl::InternalError(
        absl::StrFormat("Umbilical %s has not been preconditioned", name_));
  }
  logger_.Log(toolbelt::LogLevel::kInfo, "Umbilical %s connecting to %s (%s)",
              name_.c_str(), compute_->name.c_str(), compute_->addr.ToString().c_str());
  client_->Reset();
  if (absl::Status status =
          client_->Init(compute_->addr, name_, event_mask, compute_->name, c);
      !status.ok()) {
    return absl::InternalError(
        absl::StrFormat("Failed to connect umbilical %s to %s: %s", name_,
                        compute_->name, status.ToString()));
  }
  state_ = UmbilicalState::kConnected;
  logger_.Log(toolbelt::LogLevel::kInfo, "Umbilical %s connected to %s",
              name_.c_str(), compute_->name.c_str());
  return absl::OkStatus();
}

void Umbilical::Disconnect(bool dynamic_only) {
  dynamicRefs_--;
  if (dynamic_only && is_static_) {
    return;
  }
  if (dynamicRefs_ > 0) {
    return;
  }
  client_->Close();
  client_->Reset();
  state_ = UmbilicalState::kClosed;
}

} // namespace adastra::capcom
