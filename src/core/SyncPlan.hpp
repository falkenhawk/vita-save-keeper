#pragma once

#include <cstddef>
#include <string>

namespace vsm {

// Inputs for one game's batch decision, precomputed by the caller: folder signatures and archive
// matching need real IO, the decision itself stays pure and host-testable.
struct SyncItemInput {
  bool entries_ok{};
  bool folder_empty{};
  // Content identical to any existing local archive, not just the newest: matching an older one
  // still means the bytes are preserved, and re-zipping them would only duplicate an archive
  // under a new timestamp.
  bool matches_existing{};
  std::string newest_local;
  bool newest_on_drive{};
  bool drive_connected{};
};

struct SyncItemPlan {
  bool unreadable{};
  bool create_backup{};
  // Existing archive to upload; empty when none is due. When create_backup is set the fresh
  // archive uploads instead - its name is only known after creation.
  std::string upload_existing;
  bool will_upload{};
};

SyncItemPlan plan_sync_item(const SyncItemInput &input);

// Confirmation prompt for the batch; shown instantly on the hold gesture. Per-game work is
// decided during the run itself, so there is no scan phase to sit through before confirming.
std::string sync_all_confirm_message(std::size_t games, const std::string &tab_label,
                                     bool drive_connected);

struct SyncRunCounts {
  std::size_t backed_up{};
  std::size_t uploaded{};
  std::size_t up_to_date{};
  std::size_t failed{};
  // Games not reached because the user canceled mid-run; zero for a completed run.
  std::size_t games_left{};
};

std::string sync_run_summary(const SyncRunCounts &counts);

std::string sync_all_hold_message(const std::string &tab_label);

} // namespace vsm
