#pragma once

#include "stagezero/proto/config.pb.h"
#include "stagezero/proto/control.pb.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "coroutine.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "stagezero/symbols.h"

#include <memory>

namespace stagezero {

class ZygoteCore {
public:
  ZygoteCore(int argc, char** argv);
  ~ZygoteCore() = default;

  absl::Status Run();

private:
  static constexpr size_t kBufferSize = 4096;
  void WaitForSpawn(co::Coroutine *c);

  absl::Status HandleSpawn(const control::SpawnRequest &req,
                   control::SpawnResponse *resp,
                   std::vector<toolbelt::FileDescriptor>&& fds);

  [[noreturn]] void InvokeMainAfterSpawn(const control::SpawnRequest&& req, SymbolTable&& local_symbols);

  std::vector<std::string> args_;
  std::unique_ptr<toolbelt::UnixSocket> control_socket_;
  std::unique_ptr<co::Coroutine> server_;
  std::unique_ptr<co::Coroutine> monitor_;
  char buffer_[kBufferSize];
  toolbelt::Logger logger_;
  SymbolTable global_symbols_;
};

} // namespace stagezero