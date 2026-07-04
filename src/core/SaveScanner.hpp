#pragma once

#include "core/SaveRecord.hpp"

#include <map>
#include <string>
#include <vector>

namespace vsm {

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

std::vector<SaveRecord> scan_save_roots(const std::vector<SaveRoot> &roots);
void sort_saves_by_display_name(std::vector<SaveRecord> *saves);
// newest_remote_by_folder maps normalized save folder names to the newest remote backup name
// (timestamped names compare lexically); saves without a remote backup sort last.
void apply_save_sort(std::vector<SaveRecord> *saves, SaveSortMode mode,
                     const std::map<std::string, std::string> &newest_remote_by_folder);
const char *save_sort_mode_label(SaveSortMode mode);
std::string save_sort_mode_to_string(SaveSortMode mode);
SaveSortMode save_sort_mode_from_string(const std::string &value);

} // namespace vsm
