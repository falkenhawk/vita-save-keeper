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
  // Stat-level fingerprint of the save folder, filled at scan time for every save. It is the
  // freshness key for cached times and sfo-derived titles: both can be trusted while it matches.
  SaveFingerprint fingerprint;
  // True when the scan filled the title fields from the title cache; such a save does not need
  // the app-database pass this launch.
  bool title_from_cache{};
  // True when the display name/icon came from the system app database (set by the db pass, or
  // restored from a cache entry). Decides a rebuilt cache entry's freshness rule: db-derived
  // entries follow the database stamp, sfo-derived ones follow the folder fingerprint.
  bool title_from_app_db{};
};

} // namespace vsm
