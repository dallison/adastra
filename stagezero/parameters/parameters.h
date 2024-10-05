#pragma once

#include "absl/status/statusor.h"
#include "common/parameters.h"
#include "toolbelt/sockets.h"

namespace stagezero {
class Parameters {
public:
  Parameters(bool events = false);
  ~Parameters() = default;

  // Set the given parameter to the value.  If it doesn't exist, it will be
  // created.
  absl::Status SetParameter(const std::string &name,
                            adastra::parameters::Value value);

  // Delete the parameter with the given name.  Error if it doesn't exist.
  absl::Status DeleteParameter(const std::string &name);

  // Get a list of all parameters, not in any particular order.
  absl::StatusOr<std::vector<std::string>> ListParameters();

  // Get the names and values of all parameters, not in any particular order.
  absl::StatusOr<
      std::vector<std::shared_ptr<adastra::parameters::ParameterNode>>>
  GetAllParameters();

  // Get the value of a parameter.  Error if it doesn't exist
  absl::StatusOr<adastra::parameters::Value>
  GetParameter(const std::string &name);

  // Does the parameter exist?
  absl::StatusOr<bool> HasParameter(const std::string &name);

  const toolbelt::FileDescriptor &GetEventFD() const { return event_fd_; }
  std::unique_ptr<adastra::parameters::ParameterEvent> GetEvent() const;

private:
  absl::Status
  SendRequestReceiveResponse(const adastra::proto::parameters::Request &req,
                             adastra::proto::parameters::Response &resp);
  bool IsOpen() const { return read_fd_.Valid() && write_fd_.Valid(); }
  toolbelt::FileDescriptor read_fd_;
  toolbelt::FileDescriptor write_fd_;
  toolbelt::FileDescriptor event_fd_;
};
} // namespace stagezero