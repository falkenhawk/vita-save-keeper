#pragma once

#include "core/SaveSlotMetadata.hpp"

#include <set>
#include <string>
#include <vector>

namespace vsm {

struct TrackedPath {
  // zip entry prefix for this directory; empty means entries sit at the archive root, which is
  // only valid for a single-path entry (matches the layout of ordinary savedata backups)
  std::string prefix;
  std::string path;
};

struct TrackedFolderEntry {
  std::string id;    // filesystem-safe, "data-" prefixed; doubles as backup + Drive folder key
  std::string title; // shown in the grid; user-editable later, folder name by default
  std::vector<TrackedPath> paths;
};

struct TrackedFoldersConfig {
  std::vector<TrackedFolderEntry> entries;
  // save ids demoted to the end of the Homebrew tab and drawn dimmed; applies to savedata
  // homebrew entries as well as tracked ones
  std::set<std::string> hidden_ids;
};

struct TrackedFoldersParseResult {
  bool ok{};
  TrackedFoldersConfig config;
};

// tracked-folders.json: {"version":1,"entries":[{id,title,paths:[{prefix,path}]}],"hidden":[ids]}.
// Unknown fields are ignored so older builds can read files written by newer ones.
TrackedFoldersParseResult parse_tracked_folders_json(const std::string &text);
std::string serialize_tracked_folders_json(const TrackedFoldersConfig &config);
// "data-" + normalize_path_component(folder_name), suffixed "-2", "-3", ... until unused.
std::string make_tracked_entry_id(const std::string &folder_name,
                                  const std::set<std::string> &taken_ids);

// Resolves each path independently via resolve_save_metadata, then keeps the result with the
// newest observed time; a path with no observed time never wins over one that has one. When
// none of the paths has an observed time (including an empty paths list), returns the first
// path's backup-clock result, same as a fresh empty savedata folder would.
SaveMetadata resolve_tracked_metadata(const std::vector<std::string> &paths,
                                      const SaveDateTime &backup_clock);

} // namespace vsm
