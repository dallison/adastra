#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "module/protobuf_module.h"
#include "proto/subspace.pb.h"
#include "toolbelt/clock.h"
#include <fstream>

template <typename T>
using RawSubscriber = stagezero::module::ZeroCopySubscriber<T>;

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

    // Open the log file.
    std::string filename = "/tmp/robot_ipc.log";
    log_file_.open(filename,
                   std::ios_base::out | std::ios_base::trunc);
    if (!log_file_) {
      std::cerr << "Failed to open log file\n";
      abort();
    }
    // Write a header to the log file.
    struct {
      char magic[15];
      char version;
    } header = {"CoffeeIntoBugs", 1};
    log_file_.write(reinterpret_cast<char*>(&header), sizeof(header));
    std::cout << "Logger recording to " << filename << std::endl;
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
          auto sub = RegisterZeroCopySubscriber<std::byte>(
              name,
              [this, name, type](
                  const stagezero::module::ZeroCopySubscriber<std::byte> &sub,
                  Message<const std::byte> msg, co::Coroutine *c) {
                absl::Span<const std::byte> span = msg;
                WriteLogEntry(name, type, span);
              });
          if (!sub.ok()) {
            std::cerr << "Failed to register subscriber for " << name << ": "
                      << sub.status() << std::endl;
          }
        }));
      }
    }
  }

  // The writes to the log file are done from coroutines so there is
  // no need to serialize them (not a thread in sight).
  template <typename T> void WriteLengthAndData(const T &s) {
    uint32_t length = uint32_t(s.size());
    log_file_.put(length & 0xff);
    log_file_.put((length >> 8) & 0xff);
    log_file_.put((length >> 16) & 0xff);
    log_file_.put((length >> 24) & 0xff);
    log_file_.write(reinterpret_cast<const char*>(s.data()), s.size());
  }

  void WriteLogEntry(const std::string &channel_name, const std::string &type,
                     absl::Span<const std::byte> data) {
    WriteLengthAndData(channel_name);
    WriteLengthAndData(type);
    WriteLengthAndData(data);
    log_file_.flush();
  }

  std::ofstream log_file_;

  std::shared_ptr<Subscriber<subspace::ChannelDirectory>> directory_;

  absl::flat_hash_set<std::string> channels_;
  absl::flat_hash_set<std::unique_ptr<co::Coroutine>> receivers_;
};

DEFINE_MODULE(Logger);
