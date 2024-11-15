#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "coroutine.h"
#include "proto/telemetry.pb.h"
#include "toolbelt/sockets.h"
#include "toolbelt/triggerfd.h"
#include <chrono>
#include <poll.h>

namespace stagezero {

class Telemetry;

// Base class for telemetry commands.  These are sent from the StageZero client
// to a running process.
struct TelemetryCommand {
  virtual ~TelemetryCommand() = default;

  // The code can be used to distinguish between different commands.  There is
  // no global code namspace, so each module should define its own codes if it
  // needs to.
  virtual int Code() const { return 0; }

  // To and from proto methods are used to serialize and deserialize the
  // command.
  virtual void ToProto(adastra::proto::telemetry::Command &proto) const {};
  virtual bool FromProto(const adastra::proto::telemetry::Command &proto) {
    return false;
  }

  // Convenience method to convert to a proto and return it without needing a
  // temporary object.
  adastra::proto::telemetry::Command AsProto() const {
    adastra::proto::telemetry::Command proto;
    ToProto(proto);
    return proto;
  }
};

// Base class for telemetry status.  These are sent from a process and will be
// delivered as events from StageZero.
struct TelemetryStatus {
  virtual ~TelemetryStatus() = default;
  virtual void ToProto(adastra::proto::telemetry::Status &proto) const {};
  virtual bool FromProto(const adastra::proto::telemetry::Status &proto) {
    return false;
  }

  // Convenience method to convert to a proto and return it without needing a
  // temporary object.
  adastra::proto::telemetry::Status AsProto() const {
    adastra::proto::telemetry::Status proto;
    ToProto(proto);
    return proto;
  }
};

// This is the base for a telemetry module.  Users should subclass this to
// provide application specific telemetry handling.
class TelemetryModule {
public:
  TelemetryModule(Telemetry &telemetry, std::string name)
      : telemetry_(telemetry), name_(std::move(name)) {}
  virtual ~TelemetryModule() = default;

  const std::string Name() const { return name_; }

  // Parse the protobuf command message and return a command object.  If the
  // type is not recognized return nullptr.
  // To check if the command is of a specific type, use the Is<> method as
  // follows:
  //   if (command.command().Is<PROTOBUF-TYPE>()) {
  //
  // For example:
  // absl::StatusOr<std::unique_ptr<TelemetryCommand>>
  // SystemTelemetry::ParseCommand(
  //  const adastra::proto::telemetry::Command &command) {
  //  if (command.command().Is<adastra::proto::telemetry::ShutdownCommand>()) {
  //    adastra::proto::telemetry::ShutdownCommand shutdown;
  //    if (!command.command().UnpackTo(&shutdown)) {
  //      return absl::InternalError("Failed to unpack shutdown command");
  //    }
  //    return std::make_unique<ShutdownCommand>(shutdown.exit_code(),
  //    shutdown.timeout_seconds());
  //  }

  virtual absl::StatusOr<std::unique_ptr<TelemetryCommand>>
  ParseCommand(const adastra::proto::telemetry::Command &command) = 0;

  // This is called when a command is received.  Subclasses should override this
  // to provide implementation for the command.  For an example of how to
  // use this, see the SystemTelemetry implementation in telemetry.cc.
  virtual absl::Status HandleCommand(std::unique_ptr<TelemetryCommand> command,
                                     co::Coroutine *c) {
    return absl::OkStatus();
  }

protected:
  Telemetry &telemetry_;
  std::string name_;
};

// This is the system telemetry module.  It provides a command to shutdown a
// process without needing to send a signal.
class SystemTelemetry : public TelemetryModule {
public:
  static constexpr int kShutdownCommand = 1;

  SystemTelemetry(Telemetry &telemetry,
                  const std::string &name = "adastra::system")
      : TelemetryModule(telemetry, name) {}

  absl::StatusOr<std::unique_ptr<TelemetryCommand>>
  ParseCommand(const adastra::proto::telemetry::Command &command) override;

  absl::Status HandleCommand(std::unique_ptr<TelemetryCommand> command,
                             co::Coroutine *c) override;
};

// System telemetry command for process shutdown.
struct ShutdownCommand : public TelemetryCommand {
  ShutdownCommand(int exit_code, int timeout_secs)
      : exit_code(exit_code), timeout_secs(timeout_secs) {}
  int Code() const override { return SystemTelemetry::kShutdownCommand; }

  void ToProto(adastra::proto::telemetry::Command &proto) const override {
    adastra::proto::telemetry::ShutdownCommand shutdown;
    shutdown.set_exit_code(exit_code);
    shutdown.set_timeout_seconds(timeout_secs);
    proto.mutable_command()->PackFrom(shutdown);
  }

  bool FromProto(const adastra::proto::telemetry::Command &proto) override {
    if (!proto.command().Is<adastra::proto::telemetry::ShutdownCommand>()) {
      return false;
    }
    adastra::proto::telemetry::ShutdownCommand shutdown;
    if (!proto.command().UnpackTo(&shutdown)) {
      return false;
    }
    exit_code = shutdown.exit_code();
    timeout_secs = shutdown.timeout_seconds();
    return true;
  }

  int exit_code;    // Exit with this status.
  int timeout_secs; // You have this long to exit before you get a signal.
};

class Telemetry {
public:
  // Common case of telemetry running in our own scheduler, invisible to the
  // user.  This can be used outside of coroutine-aware programs (like in a
  // thread).  The Run() method will block until the telemetry system is
  // shutdown.
  Telemetry() { Init(local_scheduler_, &local_coroutines_); }

  // Specialized case where the telemetry is running in the provided scheduler.
  // You can pass 'coroutines' if your program holds onto coroutines in a
  // container.  The Run() method will add a coroutine to the scheduler and
  // return.
  Telemetry(co::CoroutineScheduler &scheduler,
            absl::flat_hash_set<std::unique_ptr<co::Coroutine>> *coroutines =
                nullptr) {
    Init(scheduler, coroutines);
  }

  ~Telemetry() = default;

  // Not copyable or movable.
  Telemetry(const Telemetry &) = delete;
  Telemetry &operator=(const Telemetry &) = delete;
  Telemetry(Telemetry &&) = delete;
  Telemetry &operator=(Telemetry &&) = delete;

  // Run the telemetry system.  If this is running in its own scheduler, this
  // will block until the system is shutdown.  If it's running in a user
  // provided scheduler, it will just add a coroutine and return. See the
  // constructors for details.
  void Run();

  // Ask the telemetry system to shutdown.  This will cause all our
  // coroutines to stop and, if we are in our own scheduler, the Run() method to
  // return.
  void Shutdown() { shutdown_fd_.Trigger(); }

  // Add a telemetry module to the system.
  void AddModule(std::shared_ptr<TelemetryModule> module) {
    modules_[module->Name()] = module;
  }

  absl::Status SendStatus(const TelemetryStatus &status,
                          co::Coroutine *c = nullptr) {
    return SendStatus(status.AsProto(), c);
  }

  absl::Status SendStatus(const adastra::proto::telemetry::Status &status,
                          co::Coroutine *c = nullptr);

  bool IsOpen() const { return read_fd_.Valid() && write_fd_.Valid(); }

  // Call the callback every 'period' nanoseconds.
  void CallEvery(std::chrono::nanoseconds period,
                 std::function<void(co::Coroutine *)> callback);

  // Call the callback once after 'timeout' nanoseconds.
  void CallAfter(std::chrono::nanoseconds timeout,
                 std::function<void(co::Coroutine *)> callback);

  // Call the callback now (for some definiton of "now").
  void CallNow(std::function<void(co::Coroutine *)> callback) {
    CallAfter(std::chrono::nanoseconds(0), std::move(callback));
  }

private:
  void Init(co::CoroutineScheduler &scheduler,
            absl::flat_hash_set<std::unique_ptr<co::Coroutine>> *coroutines);

  bool Wait(co::Coroutine *c);
  absl::Status HandleCommand(co::Coroutine *c);

  void AddCoroutine(std::unique_ptr<co::Coroutine> c) {
    coroutines_->insert(std::move(c));
  }

  // If we are running our own scheduler, use this one.
  co::CoroutineScheduler local_scheduler_;

  // This is scheduler we are running on (might be local_scheduler_)
  co::CoroutineScheduler *scheduler_ = nullptr;
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> *coroutines_ = nullptr;

  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> local_coroutines_;

  toolbelt::FileDescriptor read_fd_;
  toolbelt::FileDescriptor write_fd_;
  toolbelt::TriggerFd shutdown_fd_;
  bool running_ = false;
  absl::flat_hash_map<std::string, std::shared_ptr<TelemetryModule>> modules_;
};

} // namespace stagezero
