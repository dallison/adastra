#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "coroutine.h"
#include "proto/telemetry.pb.h"
#include "toolbelt/sockets.h"

namespace stagezero {

class Telemetry;

// Base class for telemetry commands.
struct TelemetryCommand {
  virtual ~TelemetryCommand() = default;
  virtual int Code() const = 0;
  virtual void ToProto(adastra::proto::telemetry::Command &proto) const {};
  virtual bool FromProto(const adastra::proto::telemetry::Command &proto) {
    return false;
  }
};

// Base class for telemetry status.
struct TelemetryStatus {
  virtual ~TelemetryStatus() = default;
  virtual void ToProto(adastra::proto::telemetry::Status &proto) const {};
  virtual bool FromProto(const adastra::proto::telemetry::Status &proto) {
    return false;
  }
};

class TelemetryModule {
public:
  TelemetryModule(Telemetry &telemetry, std::string name)
      : telemetry_(telemetry), name_(std::move(name)) {}
  virtual ~TelemetryModule() = default;

  const std::string Name() const { return name_; }

  virtual absl::StatusOr<std::unique_ptr<TelemetryCommand>>
  ParseCommand(const adastra::proto::telemetry::Command &command) = 0;

protected:
  Telemetry &telemetry_;
  std::string name_;
};

struct DiagnosticReport;

class SystemTelemetry : public TelemetryModule {
public:
  static constexpr int kShutdownCommand = 1;
  static constexpr int kDiagnosticsCommand = 2;

  SystemTelemetry(Telemetry &telemetry)
      : TelemetryModule(telemetry, "adstra::system") {}

  absl::StatusOr<std::unique_ptr<TelemetryCommand>>
  ParseCommand(const adastra::proto::telemetry::Command &command) override;

  absl::Status SendDiagnosticReport(const DiagnosticReport &diag,
                                    co::Coroutine *c = nullptr);
};

// System telemetry command for process shutdown.
struct ShutdownCommand : public TelemetryCommand {
  ShutdownCommand(int exit_code, int timeout_secs)
      : exit_code(exit_code), timeout_secs(timeout_secs) {}
  int Code() const override { return SystemTelemetry::kShutdownCommand; }
  void ToProto(adastra::proto::telemetry::Command &proto) const override {
    proto.set_code(adastra::proto::telemetry::SHUTDOWN_COMMAND);
    adastra::proto::telemetry::ShutdownCommand shutdown;
    shutdown.set_exit_code(exit_code);
    shutdown.set_timeout_seconds(timeout_secs);
    proto.mutable_data()->PackFrom(shutdown);
  }

  bool FromProto(const adastra::proto::telemetry::Command &proto) override {
    if (proto.code() != adastra::proto::telemetry::SHUTDOWN_COMMAND) {
      return false;
    }
    adastra::proto::telemetry::ShutdownCommand shutdown;
    if (!proto.data().UnpackTo(&shutdown)) {
      return false;
    }
    exit_code = shutdown.exit_code();
    timeout_secs = shutdown.timeout_seconds();
    return true;
  }

  int exit_code;    // Exit with this status.
  int timeout_secs; // You have this long to exit before you get a signal.
};

struct DiagnosticCommand : public TelemetryCommand {
  int Code() const override { return SystemTelemetry::kDiagnosticsCommand; }
  void ToProto(adastra::proto::telemetry::Command &proto) const override {
    proto.set_code(adastra::proto::telemetry::DIAGNOSTICS_COMMAND);
    adastra::proto::telemetry::DiagnosticCommand diag;
    proto.mutable_data()->PackFrom(diag);
  }
  bool FromProto(const adastra::proto::telemetry::Command &proto) override {
    if (proto.code() != adastra::proto::telemetry::DIAGNOSTICS_COMMAND) {
      return false;
    }
    adastra::proto::telemetry::DiagnosticCommand diag;
    if (!proto.data().UnpackTo(&diag)) {
      return false;
    }
    return true;
  }
};

struct Diagnostic {
  int id;
  std::string name;
  std::string description;
  int result_percent;
  void
  ToProto(adastra::proto::telemetry::DiagnosticReport_Diagnostic *proto) const {
    proto->set_id(id);
    proto->set_name(name);
    proto->set_description(description);
    proto->set_result_percent(result_percent);
  }

  bool FromProto(
      const adastra::proto::telemetry::DiagnosticReport_Diagnostic &proto) {
    id = proto.id();
    name = proto.name();
    description = proto.description();
    result_percent = proto.result_percent();
    return true;
  }
};

struct DiagnosticReport : public TelemetryStatus {
  std::vector<Diagnostic> diagnostics;

  void ToProto(adastra::proto::telemetry::Status &proto) const override {
    proto.set_code(adastra::proto::telemetry::DIAGNOSTIC_STATUS);
    adastra::proto::telemetry::DiagnosticReport diag;
    for (const auto &d : diagnostics) {
      auto *proto_diag = diag.add_reports();
      d.ToProto(proto_diag);
    }
    proto.mutable_status()->PackFrom(diag);
  }

  bool FromProto(const adastra::proto::telemetry::Status &proto) override {
    if (proto.code() != adastra::proto::telemetry::DIAGNOSTIC_STATUS) {
      return false;
    }
    adastra::proto::telemetry::DiagnosticReport diag;
    proto.status().UnpackTo(&diag);
    for (const auto &d : diag.reports()) {
      Diagnostic diagnostic;
      diagnostic.FromProto(d);
      diagnostics.push_back(diagnostic);
    }

    return true;
  }
};

class Telemetry {
public:
  Telemetry();
  ~Telemetry() = default;

  // Not copyable or movable.
  Telemetry(const Telemetry &) = delete;
  Telemetry &operator=(const Telemetry &) = delete;
  Telemetry(Telemetry &&) = delete;
  Telemetry &operator=(Telemetry &&) = delete;

  void AddModule(std::shared_ptr<TelemetryModule> module) {
    modules_[module->Name()] = module;
  }
  // Use this to poll for commands.
  const toolbelt::FileDescriptor &GetCommandFD() const { return read_fd_; }

  // Read a command.  This will block until an event is available.
  absl::StatusOr<std::unique_ptr<TelemetryCommand>>
  GetCommand(co::Coroutine *c = nullptr) const;

  absl::Status SendStatus(const adastra::proto::telemetry::Status &status,
                          co::Coroutine *c = nullptr);
  bool IsOpen() const { return read_fd_.Valid() && write_fd_.Valid(); }

private:
  toolbelt::FileDescriptor read_fd_;
  toolbelt::FileDescriptor write_fd_;
  absl::flat_hash_map<std::string, std::shared_ptr<TelemetryModule>> modules_;
};

} // namespace stagezero
