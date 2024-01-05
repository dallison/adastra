#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_replace.h"
#include "absl/types/span.h"
#include "module/protobuf_module.h"
#include "proto/subspace.pb.h"
#include "toolbelt/clock.h"
#include <fstream>
#include <filesystem>

template <typename T>
using Subscriber = stagezero::module::ProtobufSubscriber<T>;

template <typename T> using Message = stagezero::module::Message<T>;

class Logger : public stagezero::module::ProtobufModule {
public:
  Logger(std::unique_ptr<stagezero::SymbolTable> symbols) : ProtobufModule(std::move(symbols)) {}

  absl::Status Init(int argc, char **argv) override {
    // Subscribe to the Subspace channel directory so that we can see
    // all channels.
    auto directory = RegisterSubscriber<subspace::ChannelDirectory>(
        "/subspace/ChannelDirectory",
        [this](const Subscriber<subspace::ChannelDirectory> &sub,
               Message<const subspace::ChannelDirectory> msg,
               co::Coroutine *c) { IncomingChannelDirectory(msg); });
    if (!directory.ok()) {
      return directory.status();
    }
    directory_ = std::move(*directory);

    std::filesystem::create_directory("/tmp/ipc_logs");

    return absl::OkStatus();
  }

private:
  void IncomingChannelDirectory(Message<const subspace::ChannelDirectory> dir) {
    for (auto &channel : dir->channels()) {
      if (channel.name() == "/subspace/ChannelDirectory") {
        // Don't record the channel directory.
        continue;
      }
      auto it = channels_.find(channel.name());
      if (it == channels_.end()) {
        // Unknown channel, add it.
        // We spawn a coroutine to subscribe to the channel and write all
        // messages received on it to the log file.
        std::cout << "Beginning recording of " << channel.name() << std::endl;
        channels_.insert(channel.name());

        receivers_.insert(std::make_unique<co::Coroutine>(scheduler_, [
          this, name = channel.name(), type = channel.type()
        ](co::Coroutine * c) {
          auto log_file = std::make_shared<std::ofstream>();
          if (absl::Status status = OpenLogFile(name, type, *log_file); !status.ok()) {
            std::cerr << status << std::endl;
            return;
          }
          auto sub = RegisterZeroCopySubscriber<std::byte>(
              name,
              [this, log_file](
                  const stagezero::module::ZeroCopySubscriber<std::byte> &sub,
                  Message<const std::byte> msg, co::Coroutine *c) {
             absl::Span<const std::byte> span = msg;
                if (absl::Status status = WriteLogEntry(*log_file, span); !status.ok()) {
                  std::cerr << status << std::endl;
                }
              });
          if (!sub.ok()) {
            std::cerr << "Failed to register subscriber for " << name << ": "
                      << sub.status() << std::endl;
            return;
          }
        }));
      }
    }
  }

  // The writes to the log file are done from coroutines so there is
  // no need to serialize them (not a thread in sight).
  template <typename T> void WriteLengthAndData(std::ofstream& file, const T &s) {
    uint32_t length = uint32_t(s.size());
    file.put(length & 0xff);
    file.put((length >> 8) & 0xff);
    file.put((length >> 16) & 0xff);
    file.put((length >> 24) & 0xff);
    file.write(reinterpret_cast<const char*>(s.data()), s.size());
  }

  absl::Status WriteLogEntry(std::ofstream& file,
                     absl::Span<const std::byte> data) {
    WriteLengthAndData(file, data);
    if (file.bad()) {
      return absl::InternalError("Failed to write to log file");
    }
    file.flush();
    return absl::OkStatus();
  }

 absl::Status OpenLogFile(const std::string& channel_name, const std::string& channel_type, 
  std::ofstream& file) {
    std::string filename = absl::StrFormat("/tmp/ipc_logs/IPC%s.log", absl::StrReplaceAll(channel_name, {
      {"_", "_U"},
      {"/", "_S"}
    }));
        file.open(filename,
                   std::ios_base::out | std::ios_base::trunc);
    if (!file) {
      return absl::InternalError(absl::StrFormat("Failed to open log file: %s", filename));
    }
    // Write a header to the log file.
    struct {
      char magic[15];
      char version;
    } header = {"CoffeeIntoBugs", 1};
    file.write(reinterpret_cast<char*>(&header), sizeof(header));

    // Write the channel name.
    WriteLengthAndData(file, channel_name);
    WriteLengthAndData(file, channel_type);
    return absl::OkStatus();
  }

  std::shared_ptr<Subscriber<subspace::ChannelDirectory>> directory_;

  absl::flat_hash_set<std::string> channels_;
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> receivers_;
};

DEFINE_MODULE(Logger);
