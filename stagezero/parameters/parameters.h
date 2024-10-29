#pragma once

#include "absl/status/statusor.h"
#include "common/parameters.h"
#include "coroutine.h"
#include "toolbelt/sockets.h"

namespace stagezero {
class Parameters {
public:
  Parameters(bool events = false, co::Coroutine *c = nullptr);
  ~Parameters() = default;

  // Set the given parameter to the value.  If it doesn't exist, it will be
  // created.
  absl::Status SetParameter(const std::string &name,
                            adastra::parameters::Value value,
                            co::Coroutine *c = nullptr);

  // Delete the parameter with the given name.  Error if it doesn't exist.
  absl::Status DeleteParameter(const std::string &name,
                               co::Coroutine *c = nullptr);

  // Get a list of all parameters, not in any particular order.
  absl::StatusOr<std::vector<std::string>>
  ListParameters(co::Coroutine *c = nullptr);

  // Get the names and values of all parameters, not in any particular order.
  absl::StatusOr<
      std::vector<std::shared_ptr<adastra::parameters::ParameterNode>>>
  GetAllParameters(co::Coroutine *c = nullptr);

  // Get the value of a parameter.  Error if it doesn't exist
  absl::StatusOr<adastra::parameters::Value>
  GetParameter(const std::string &name, co::Coroutine *c = nullptr);

  // Does the parameter exist?
  absl::StatusOr<bool> HasParameter(const std::string &name,
                                    co::Coroutine *c = nullptr);

  // Use this to poll for events.
  const toolbelt::FileDescriptor &GetEventFD() const { return event_fd_; }

  // Read an event.  This will block until an event is available.
  std::unique_ptr<adastra::parameters::ParameterEvent>
  GetEvent(co::Coroutine *c = nullptr) const;

private:
  absl::Status
  SendRequestReceiveResponse(const adastra::proto::parameters::Request &req,
                             adastra::proto::parameters::Response &resp,
                             co::Coroutine *c);
  bool IsOpen() const { return read_fd_.Valid() && write_fd_.Valid(); }
  toolbelt::FileDescriptor read_fd_;
  toolbelt::FileDescriptor write_fd_;
  toolbelt::FileDescriptor event_fd_;
};
} // namespace stagezero
