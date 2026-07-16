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

enum class SaveSortMode {
  Name,
  LastSaved,
  LastSynced,
};

constexpr int kSaveSortModeCount = 3;

using SaveMetadataResolver =
    std::function<SaveMetadata(const std::string &, const SaveDateTime &)>;

std::vector<SaveRecord> scan_save_roots(
    const std::vector<SaveRoot> &roots,
    const std::function<void()> &on_progress = {},
    const SaveMetadataResolver &resolve_metadata = {});
// Accepts only a real Vita slot time for a PFS-protected save. Raw filesystem times in sce_pfs
// describe encryption bookkeeping and must never become user-facing save times.
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
