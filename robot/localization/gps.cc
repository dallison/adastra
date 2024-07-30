#include "absl/strings/str_format.h"
#include "module/ros_module.h"
#include "robot/ros_msgs/zeros/robot_msgs/GpsLocation.h"
#include "toolbelt/clock.h"

template <typename T> using Publisher = adastra::module::ZerosPublisher<T>;

template <typename T>
using Subscriber = adastra::module::ZerosSubscriber<T>;

template <typename T> using Message = adastra::module::Message<T>;

using namespace adastra::module::frequency_literals;

// This module sends GPS messages at 2Hz.  The messages are sent
// as zero-copy ROS messages on the /gps channel.
class GpsReceiver : public adastra::module::ROSModule {
public:
  explicit GpsReceiver(std::unique_ptr<adastra::stagezero::SymbolTable> symbols)
      : Module(std::move(symbols)) {}

  absl::Status Init(int argc, char **argv) override {
    constexpr uint64_t kMaxMessageSize = 32;
    constexpr int32_t kNumSlots = 16;

    // Let's go north at 60 miles per hour.  1 degree of latitude is about
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

    auto pub = RegisterPublisher<robot_msgs::zeros::GpsLocation>(
        "/gps", kMaxMessageSize, kNumSlots,
        [this](auto pub, auto &msg, auto c) -> size_t {
          auto now = toolbelt::Now();
          davros::Time timestamp = {uint32_t(now / 1000000000), uint32_t(now % 1000000000)};
          msg.header->timestamp = timestamp;
          msg.latitude = lat_;
          msg.longitude = long_;
          // Update latitude.
          lat_ += kDegreesPerSec / kFrequency;
          return msg.BinarySize();
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
  static constexpr double kLat = 37.897462;
  static constexpr double kLong = -122.063484;

  double lat_ = kLat;
  double long_ = kLong;
};

DEFINE_MODULE(GpsReceiver);
