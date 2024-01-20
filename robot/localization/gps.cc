#include "absl/strings/str_format.h"
#include "module/protobuf_module.h"
#include "robot/proto/gps.pb.h"
#include "toolbelt/clock.h"

template <typename T> using Publisher = stagezero::module::ProtobufPublisher<T>;

template <typename T>
using Subscriber = stagezero::module::ProtobufSubscriber<T>;

template <typename T> using Message = stagezero::module::Message<T>;

using namespace stagezero::module::frequency_literals;

class GpsReceiver : public stagezero::module::ProtobufModule {
public:
  GpsReceiver(std::unique_ptr<stagezero::SymbolTable> symbols)
      : ProtobufModule(std::move(symbols)) {}

  absl::Status Init(int argc, char **argv) override {
    constexpr uint64_t kMaxMessageSize = 32;
    constexpr int32_t kNumSlots = 16;

    // Let's go north at 60 miles per hour.  I degree of latitude is about
    // 69 miles, so if we send one GPS message per second we will have moved
    // 1/60 miles.
    // 69 miles = 1 degrees
    // 1 mile = 1/69 degrees
    // 1/60 miles = 1/(69*60) degrees
    // = 0.00024154 degrees per second

    // Degrees per second latitude change at 60mph.
    constexpr double kDegreesPerSec = 0.00024154;

    // Frequency we will send gps at.
    constexpr double kFrequency = 2_hz;

    auto pub = RegisterPublisher<robot::GpsLocation>(
        "/gps", kMaxMessageSize, kNumSlots,
        [this](auto pub, auto &msg, auto c) -> bool {
          msg.mutable_header()->set_timestamp(toolbelt::Now());
          msg.set_latitude(lat_);
          msg.set_longitude(long_);
          // Update latitude.
          lat_ += kDegreesPerSec / kFrequency;
          return true;
        });
    if (!pub.ok()) {
      return pub.status();
    }

    // Send the gps location at 2Hz.
    RunPeriodically(
        kFrequency, [pub = *pub](co::Coroutine * c) { pub->Publish(); });
    return absl::OkStatus();
  }

private:
  // Start out in sunny California.
  static constexpr double kLat = 37.777080;
  static constexpr double kLong = -121.967520;

  double lat_ = kLat;
  double long_ = kLong;
};

DEFINE_MODULE(GpsReceiver);
