// Copyright 2024 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#pragma once

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "coroutine.h"
#include "proto/config.pb.h"
#include "proto/control.pb.h"
#include "stagezero/symbols.h"
#include "toolbelt/logging.h"
#include "toolbelt/sockets.h"
#include "toolbelt/pipe.h"

#include <memory>

namespace adastra::stagezero {

class ZygoteCore {
 public:
  ZygoteCore(int argc, char** argv);
  ~ZygoteCore() = default;

  absl::Status Run();

 private:
  static constexpr size_t kBufferSize = 4096;
  void WaitForSpawn(co::Coroutine* c);

  absl::Status HandleSpawn(const control::SpawnRequest& req,
                           control::SpawnResponse* resp,
                           std::vector<toolbelt::FileDescriptor>& fds,
                           co::Coroutine* c);

  [[noreturn]] static void InvokeMainAfterSpawn(std::string exe, const control::SpawnRequest&& req,
                                         std::unique_ptr<SymbolTable> local_symbols);

  void Run(const std::string& dso, const std::string& main, const std::vector<std::string>& vars);
  
  co::CoroutineScheduler scheduler_;
  std::vector<std::string> args_;
  std::unique_ptr<toolbelt::UnixSocket> control_socket_;
  toolbelt::FileDescriptor notification_pipe_;
  std::unique_ptr<co::Coroutine> server_;
  std::unique_ptr<co::Coroutine> monitor_;
  char buffer_[kBufferSize];
  toolbelt::Logger logger_;
  SymbolTable global_symbols_;

  bool forked_ = false;

  // After the fork, this struct holds the information we need to
  // invoke the virtual process.  It is only in the child process.
  struct AfterFork {
    control::SpawnRequest req;
    std::unique_ptr<SymbolTable> local_symbols;
  } after_fork_;
};

}  // namespace adastra::stagezero
