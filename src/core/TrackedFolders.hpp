#pragma once

#include "core/SaveSlotMetadata.hpp"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace vsm {

// Defined in core/SaveRecord.hpp, which includes this header; kept forward-declared here so this
// header never depends back on SaveRecord.hpp. (TrackedPath now lives in SaveSlotMetadata.hpp, the
// header included above, so SaveMetadata can record a tracked backup's restore mapping.)
struct SaveRecord;

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
// Temp file then rename, same as the metadata sidecars (see SaveTimeCache.cpp's write_json_atomic):
// a reader never sees a half-written file, so a torn write (power loss, full storage) can never
// corrupt the user's only copy of tracked-folders.json.
bool write_tracked_folders_json_atomic(const std::string &path, const TrackedFoldersConfig &config,
                                       std::string *error);
// "data-" + normalize_path_component(folder_name), suffixed "-2", "-3", ... until unused.
std::string make_tracked_entry_id(const std::string &folder_name,
                                  const std::set<std::string> &taken_ids);

// Validates restore targets read from a per-backup sidecar before they are trusted to drive a
// restore, which CLEARS each destination directory. A sidecar is user-editable JSON, so every
// destination must be confined to ux0:data (where tracked folders live) with no ".." segment that
// could climb out, and the prefixes must be well-formed the same way the archive writer/reader
// require (distinct, empty prefix only when it is the sole target). An empty list is vacuously safe;
// callers decide separately whether an empty mapping is usable.
bool tracked_targets_are_safe(const std::vector<TrackedPath> &targets);

// Resolves each path independently via resolve_save_metadata, then keeps the result with the
// newest observed time; a path with no observed time never wins over one that has one. When
// none of the paths has an observed time (including an empty paths list), returns the first
// path's backup-clock result, same as a fresh empty savedata folder would.
SaveMetadata resolve_tracked_metadata(const std::vector<std::string> &paths,
                                      const SaveDateTime &backup_clock);

// Turns tracked-folder config into first-class save records for the Homebrew tab. The builtin
// RetroArch entry (savefiles + savestates under ux0:data/retroarch) is synthesized first when its
// directory exists; a config entry reusing its "data-retroarch" id is dropped so a stale
// hand-edited file can never duplicate it, whether or not the builtin's directory is present.
// Save times are left unresolved (save_time_known false); that happens later at the app layer.
std::vector<SaveRecord>
build_tracked_save_records(const TrackedFoldersConfig &config,
                           const std::function<bool(const std::string &)> &directory_exists);

} // namespace vsm
