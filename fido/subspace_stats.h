#pragma once

#include "fido/screen.h"
#include "fido/table_window.h"
#include "client/client.h"
#include "proto/subspace.pb.h"

namespace fido {

class SubspaceStatsWindow : public TableWindow {
public:
  SubspaceStatsWindow(Screen *screen, const std::string& subspace_socket);
  ~SubspaceStatsWindow() = default;
  
private:
  struct Stats {
    uint64_t sample_time;
    int64_t bytes;
    int64_t msgs;
    int64_t time_diff;
    int64_t bytes_diff;
    int64_t msgs_diff;
    int32_t slot_size;
    int32_t num_slots;
    int32_t num_pubs;
    int32_t num_subs;
  };

  void RunnerCoroutine(co::Coroutine *c) override;
  void IncomingChannelStats(const subspace::Statistics& stats);
  void PopulateTable();
  void AgerCoroutine(co::Coroutine* c);

  std::string subspace_socket_;
  std::unique_ptr<subspace::Client> client_;
  absl::flat_hash_map<std::string, Stats> channels_;
};

} // namespace fido