#include "fido/subspace_stats.h"
#include "absl/strings/str_format.h"
#include "fido/fido.h"
#include "toolbelt/clock.h"

#include <fstream>
#include <stdlib.h>

namespace adastra::fido {

SubspaceStatsWindow::SubspaceStatsWindow(retro::Screen *screen,
                                         const std::string &subspace_socket)
    : TableWindow(screen,
                  {.title = "[3] Subspace Channels",
                   .nlines = 16,
                   .ncols = screen->Width() * 9 / 16 - 1,
                   .x = 0,
                   .y = 17},
                  {"channel", "freq", "bps", "bytes", "msgs", "size", "slots",
                   "pubs", "subs"}),
      subspace_socket_(subspace_socket) {
  // Sort the display by frequency with the highest frequency at the top.
  display_table_.SortBy(1,
                        [](const std::string &a, const std::string &b) -> bool {
                          // The display for this column is "%.2fXHz".  The
                          // strtod function conveniently ignores anything not
                          // part of a floating point number.
                          if (a.find("nan") != std::string::npos ||
                              b.find("nan") != std::string::npos) {
                            return false;
                          }
                          if (a.empty() || b.empty()) {
                            return false;
                          }
                          double x = strtod(a.c_str(), nullptr);
                          double y = strtod(b.c_str(), nullptr);
                          return x > y;
                        });
}

// This ages out channels that no longer exist.
void SubspaceStatsWindow::AgerCoroutine(co::Coroutine *c) {
  for (;;) {
    uint64_t before_sleep = toolbelt::Now();
    c->Sleep(3); // Stats are sent every 2 seconds.

    // If we didn't receive any stats updates in 3 seconds, the channel
    // has gone away.
    for (auto it = channels_.begin(); it != channels_.end();) {
      if (it->second.sample_time < before_sleep) {
        channels_.erase(it++);
      } else {
        it++;
      }
    }
    Draw();
  }
}

void SubspaceStatsWindow::RunnerCoroutine(co::Coroutine *c) {
  bool connected = false;

  // Start the ager coroutine.
  App().AddCoroutine(std::make_unique<co::Coroutine>(
      Scheduler(), [this](co::Coroutine *c) { AgerCoroutine(c); }));

  absl::StatusOr<subspace::Subscriber> sub;
  for (;;) {
    if (!connected) {
      DrawErrorBanner("CONNECTING TO SUBSPACE");
      client_ = std::make_unique<subspace::Client>(c);
      if (absl::Status status = client_->Init(subspace_socket_); !status.ok()) {
        c->Sleep(2);
        Draw();
        continue;
      }

      sub = client_->CreateSubscriber("/subspace/Statistics");
      if (!sub.ok()) {
        c->Sleep(2);
        continue;
      }
      connected = true;
    }
    if (absl::Status status = sub->Wait(c); !status.ok()) {
      connected = false;
      client_.reset();
      continue;
    }
    for (;;) {
      absl::StatusOr<const subspace::Message> msg = sub->ReadMessage();
      if (!msg.ok()) {
        connected = false;
        client_.reset();
        continue;
      }
      if (msg->length == 0) {
        break;
      }
      subspace::Statistics stats;
      if (!stats.ParseFromArray(msg->buffer, msg->length)) {
        connected = false;
        client_.reset();
        continue;
      }
      IncomingChannelStats(stats);
    }
  }
}

void SubspaceStatsWindow::IncomingChannelStats(
    const subspace::Statistics &stats) {
  uint64_t sample_time = stats.timestamp();
  for (auto &s : stats.channels()) {
    const std::string &name = s.channel_name();
    auto it = channels_.find(name);
    if (it == channels_.end()) {
      // Unknown channel, add it.
      Stats st = {.sample_time = sample_time,
                  .bytes = s.total_bytes(),
                  .msgs = s.total_messages(),
                  .slot_size = s.slot_size(),
                  .num_slots = s.num_slots(),
                  .num_pubs = s.num_pubs(),
                  .num_subs = s.num_subs()};
      channels_.insert(std::make_pair(name, st));
    } else {
      // Channel is known, calculate difference from previous stats.
      uint64_t time_diff = sample_time - it->second.sample_time;
      uint64_t bytes_diff = s.total_bytes() - it->second.bytes;
      uint64_t msgs_diff = s.total_messages() - it->second.msgs;
      it->second.sample_time = sample_time;
      it->second.bytes = s.total_bytes();
      it->second.msgs = s.total_messages();
      it->second.time_diff = time_diff;
      it->second.bytes_diff = bytes_diff;
      it->second.msgs_diff = msgs_diff;
      it->second.slot_size = s.slot_size();
      it->second.num_slots = s.num_slots();
      it->second.num_pubs = s.num_pubs();
      it->second.num_subs = s.num_subs();
    }
  }
  PopulateTable();
}

static std::string ToHz(double f) {
  if (f < 1000) {
    return absl::StrFormat("%.2fHz", f);
  }
  if (f < 1000000) {
    return absl::StrFormat("%.2fKHz", f / 1000000.0);
  }
  if (f < 1000000000) {
    return absl::StrFormat("%.2fMHz", f / 1000000000.0);
  }
  return absl::StrFormat("%.2fGHz", f / 1000000000000.0);
}

void SubspaceStatsWindow::ApplyFilter() { PopulateTable(); }

void SubspaceStatsWindow::PopulateTable() {
  retro::Table &table = display_table_;
  table.Clear();
  for (auto & [ name, stats ] : channels_) {
    if (!display_filter_.empty() &&
        name.find(display_filter_) == std::string::npos) {
      continue;
    }
    table.AddRow();

    // Time diff is in nanoseconds.
    double freq = stats.msgs_diff * 1000000000.0 / stats.time_diff;
    double bps = stats.bytes_diff * 1000000000.0 / stats.time_diff;
    int freq_color = freq > 0 ? retro::kColorPairGreen : retro::kColorPairRed;
    table.SetCell(0, retro::Table::MakeCell(name));
    table.SetCell(1, retro::Table::MakeCell(ToHz(freq), freq_color));
    table.SetCell(2, retro::Table::MakeCell(absl::StrFormat("%.2g", bps)));
    table.SetCell(3, retro::Table::MakeCell(
                         absl::StrFormat("%.2g", double(stats.bytes))));
    table.SetCell(4, retro::Table::MakeCell(absl::StrFormat("%d", stats.msgs)));
    table.SetCell(5, retro::Table::MakeCell(
                         absl::StrFormat("%.2g", double(stats.slot_size))));
    table.SetCell(
        6, retro::Table::MakeCell(absl::StrFormat("%d", stats.num_slots)));
    table.SetCell(
        7, retro::Table::MakeCell(absl::StrFormat("%d", stats.num_pubs)));
    table.SetCell(
        8, retro::Table::MakeCell(absl::StrFormat("%d", stats.num_subs)));
  }
  Draw();
}

} // namespace adastra::fido