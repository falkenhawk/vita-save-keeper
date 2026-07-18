#pragma once

#include "core/SaveSlotMetadata.hpp"
#include "core/SaveTimeCache.hpp"

#include <string>

namespace vsm {

enum class SavePlatform {
  Vita,
  GameCard,
  Psp,
};

struct SaveRecord {
  SavePlatform platform{};
  std::string id;
  std::string title_id;
  std::string display_name;
  std::string path;
  std::string icon_path;
  // Resolved from the newest valid Vita slot, falling back to the newest recursively scanned
  // regular file for formats without slot metadata.
  SaveDateTime saved_at;
  long long saved_at_epoch{};
  // False when no trustworthy save time was found and saved_at_epoch only contains a scan-time
  // fallback. The UI must not present that fallback as the time the game was saved.
  bool save_time_known{};
  // PFS bookkeeping files have unrelated modification times. Vita code resolves these saves once
  // through a decrypted mount before exposing or sorting by their actual slot time.
  bool save_time_requires_mount{};
  // Stat-level fingerprint of the save folder, filled at scan time for mount-requiring saves so
  // a cached mount-resolved time can be trusted while the folder is unchanged.
  SaveFingerprint fingerprint;
};

} // namespace vsm
