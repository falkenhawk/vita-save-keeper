#pragma once

#include "core/SaveRecord.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace vsm {

struct SaveMetadata;

struct SaveRoot {
  SavePlatform platform{};
  std::string path;
};

// Declaration order is the tap-cycle order: cycle_sort_mode steps (mode + 1) % count. Persisted
// settings map by string, so reordering here does not disturb saved preferences.
enum class SaveSortMode {
  Name,
  LastBackup,
  LastSaved,
};

constexpr int kSaveSortModeCount = 3;

using SaveMetadataResolver =
    std::function<SaveMetadata(const std::string &, const SaveDateTime &)>;
// Called after each save is processed with (saves done, total to do), so a caller can show a real
// percentage while scanning. The total is known up front from the directory listing.
using SaveScanProgress = std::function<void(std::size_t done, std::size_t total)>;

// time_cache and title_cache are optional accelerators: a save whose folder fingerprint matches
// its time entry skips the metadata resolver, and one whose title entry is valid (see
// SaveTitleCacheEntry) skips the param.sfo reads, with title_from_cache set so the caller knows
// the app-database pass is not needed for it. The scan never writes the caches; callers rebuild
// entries from the returned records.
std::vector<SaveRecord> scan_save_roots(
    const std::vector<SaveRoot> &roots,
    const SaveScanProgress &on_progress = {},
    const SaveMetadataResolver &resolve_metadata = {},
    const SaveTimeCache *time_cache = nullptr,
    const SaveTitleCache *title_cache = nullptr);
// Applies a save time resolved through the live mount: a real Vita slot time when the save has
// slots, otherwise the newest-file time as an approximate fallback. Only a backup-clock (no
// observed time) result is rejected, leaving the save time unknown. Matches what the details
// screen shows for the same save.
bool apply_mounted_save_time(SaveRecord *save, const SaveMetadata &metadata);
void sort_saves_by_display_name(std::vector<SaveRecord> *saves);
// newest_remote_by_folder maps normalized save folder names to the newest remote backup name
// (timestamped names compare lexically); saves without a remote backup sort last.
void apply_save_sort(std::vector<SaveRecord> *saves, SaveSortMode mode,
                     const std::map<std::string, std::string> &newest_remote_by_folder);
bool save_sort_requires_all_times(SaveSortMode mode);
const char *save_sort_mode_label(SaveSortMode mode);
std::string save_sort_mode_to_string(SaveSortMode mode);
SaveSortMode save_sort_mode_from_string(const std::string &value);

} // namespace vsm
