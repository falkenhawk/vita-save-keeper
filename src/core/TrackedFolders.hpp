#pragma once

#include "core/SaveSlotMetadata.hpp"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace vsm {

// Defined in core/SaveRecord.hpp, which includes this header; kept forward-declared here so this
// header never depends back on SaveRecord.hpp. (TrackedPath lives in SaveSlotMetadata.hpp, the
// header included above, so SaveMetadata can record a backup's extra restore mapping.)
struct SaveRecord;

// Zip prefix reserved for an entry's own savedata folder once an archive holds more than one
// directory. It is deliberately never written to a sidecar: the app maps it to the entry's live
// path, so a hand-edited sidecar can only ever redirect the extra ux0:data folders, never aim a
// directory-clearing restore at the savedata partition.
extern const char *const kSavedataPrefix;

// One homebrew app configured to fold extra ux0:data folders into its own backup.
struct TrackedFolderEntry {
  // The app's save id: its savedata folder name, or its title id when it has no savedata folder.
  // Never synthetic - an entry is always an app that exists.
  std::string id;
  // Fallback name, used only for an app the system database cannot name. Usually empty, because
  // the app.db pass supplies the real title and icon.
  std::string title;
  // Extra folders, each with the zip prefix allocated when it was attached.
  std::vector<TrackedPath> paths;
};

struct TrackedFoldersConfig {
  std::vector<TrackedFolderEntry> entries;
  // Save ids left out of the hold-Select batch sweep. Applies to any homebrew entry, whether or
  // not it has extra folders - the point is keeping settings noise out of the sweep.
  std::set<std::string> skipped_ids;
  // UTC epoch seconds of the last local change, stamped on every mutation. The Drive sync compares
  // it across devices (last writer wins), so it must be wall-clock UTC, not local time. 0 in the
  // shipped base file and on a device that never changed anything.
  long long modified{};
};

struct TrackedFoldersParseResult {
  bool ok{};
  TrackedFoldersConfig config;
};

// {"version":1,"modified":epoch,"entries":[{id,title,paths:[{prefix,path}]}],"skipped":[ids]}.
// One schema serves both the base file shipped inside the VPK (savedata-paths.json: known folder
// sets, RetroArch first) and the user's backup-settings.json, which overrides it per entry id and
// adds the skip list. An entry with an EMPTY paths array is a tombstone: in the user file it
// suppresses the base entry with that id. Unknown fields are ignored so older builds can read
// files written by newer ones.
TrackedFoldersParseResult parse_tracked_folders_json(const std::string &text);
std::string serialize_tracked_folders_json(const TrackedFoldersConfig &config);
// Temp file then rename, same as the metadata sidecars (see SaveTimeCache.cpp's write_json_atomic):
// a reader never sees a half-written file, so a torn write (power loss, full storage) can never
// corrupt the user's only copy of backup-settings.json.
bool write_tracked_folders_json_atomic(const std::string &path, const TrackedFoldersConfig &config,
                                       std::string *error);

// Zip prefix for a newly attached folder: normalize_path_component of its name, suffixed "-2",
// "-3", ... until unused. kSavedataPrefix is always treated as taken, so a folder literally named
// "savedata" cannot collide with the entry's own savedata tree. Allocated once at attach time and
// stored in the config, so renaming a folder later can never repoint an existing backup's mapping.
std::string make_extra_prefix(const std::string &folder_name,
                              const std::set<std::string> &taken_prefixes);

// How one entry's directories are laid out in its archive.
struct ArchiveLayout {
  // Every directory to zip, in archive order. Exactly one source with an empty prefix when the
  // entry has a single directory, which keeps the layout byte-identical to an ordinary savedata
  // backup so existing archives and dedup signatures still match. Otherwise the savedata folder
  // sits under kSavedataPrefix and each extra under its own.
  std::vector<TrackedPath> sources;
  // The subset a backup's sidecar records: everything except the savedata folder. Empty for an
  // entry with no extras, which is what keeps ordinary savedata sidecars byte-identical.
  std::vector<TrackedPath> extra_targets;
};

ArchiveLayout archive_layout_for_record(const SaveRecord &record);

// Restore mapping for an archive that carries extra folders: the sidecar's extra targets plus the
// savedata destination, which the caller supplies from the live entry rather than the file. An
// empty savedata_path (an app that has no savedata folder) contributes nothing. Returns an empty
// vector when extra_targets is empty, which tells the caller to restore the flat, single-directory
// way - the path every archive written before this feature takes.
std::vector<TrackedPath> restore_targets_for_backup(const std::string &savedata_path,
                                                    const std::vector<TrackedPath> &extra_targets);

// Validates restore targets read from a per-backup sidecar before they are trusted to drive a
// restore, which CLEARS each destination directory. A sidecar is user-editable JSON, so every
// destination must be confined to ux0:data (where extra folders live) with no ".." segment that
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

// Order-insensitive equality of two folder sets (prefix+path pairs). Order carries no meaning in
// an archive or a restore, so it must not keep a user override alive.
bool tracked_paths_equal(const std::vector<TrackedPath> &a, const std::vector<TrackedPath> &b);

// The folder sets that actually apply: the base file's entries overlaid by the user's. A base
// entry only applies while at least one of its folders exists (so an uninstalled RetroArch never
// grows a dead row); a user entry with the same id replaces it wholesale, ungated (the user chose
// those folders and needs to see them to exclude them); a user tombstone (empty paths) suppresses
// it. Entries that end up with no paths are dropped from the result.
std::vector<TrackedFolderEntry>
effective_data_folder_entries(const TrackedFoldersConfig &base, const TrackedFoldersConfig &user,
                              const std::function<bool(const std::string &)> &directory_exists);

// Applies one UI change to the USER config so the base stays authoritative wherever possible:
// new_paths equal to the applicable base entry (base_paths, null when none applies) removes the
// user override entirely - the entry falls back to the base, including whatever a future version
// ships for it. An empty new_paths writes a tombstone over a base entry, or just removes a purely
// user entry. Anything else is stored as the override.
void update_data_folder_override(TrackedFoldersConfig *user, const std::string &id,
                                 const std::string &title,
                                 const std::vector<TrackedPath> &new_paths,
                                 const std::vector<TrackedPath> *base_paths);

// What one Drive reconciliation should do, decided from three stamps: the local file's modified,
// the remote file's, and the modified value both sides agreed on at the last successful sync
// (device-local, kept in settings.txt). "Conflict" means both sides changed since that agreement;
// the newer stamp wins, and only when the LOCAL side loses is a conflict copy warranted - a losing
// remote version stays recoverable through Drive's own revision history.
enum class BackupSettingsSyncAction {
  InSync,
  Push,           // only local changed (or no remote exists yet and there is content to publish)
  Adopt,          // only remote changed
  PushConflict,   // both changed, local is newer - push, no conflict copy needed
  AdoptConflict,  // both changed, remote is newer - adopt, keep the local loser as a conflict copy
};
BackupSettingsSyncAction decide_backup_settings_sync(long long local_modified,
                                                     bool local_has_content, bool remote_exists,
                                                     long long remote_modified,
                                                     long long synced_modified);

// Records for config entries that no savedata scan produced - apps that keep everything in
// ux0:data and so have no savedata folder to be found by. is_scanned_id reports whether the scan
// already yielded that id, in which case the entry's folders are attached to the existing record
// instead and no new one is synthesized here. The returned records carry an empty path (there is no
// savedata folder), their extra folders, and the config title; the app.db pass fills in the real
// display name and icon afterwards.
std::vector<SaveRecord>
build_orphan_app_records(const TrackedFoldersConfig &config,
                         const std::function<bool(const std::string &)> &is_scanned_id);

// Best guess at which ux0:data child folder belongs to an app, by case-folded, separator-stripped
// comparison of the app's title against the folder names: an exact match first, then a containment
// match in either direction. Containment needs at least four normalized characters, so short names
// like "jav" cannot latch onto unrelated titles. Returns an empty string when nothing matches.
//
// This only ever chooses the folder browser's starting directory and pre-selects an owner in the
// confirm step; it never attaches anything by itself. Verified against the user's device 2026-07-25:
// it resolves 6 of 9 installed homebrew, missing abbreviations ("Save Manager" / savemgr), roman
// numerals ("AutoPlugin II" / AUTOPLUGIN2) and initialisms ("Adrenaline Bubbles Manager" / ABM).
std::string best_data_folder_match(const std::string &app_title,
                                   const std::vector<std::string> &folder_names);

} // namespace vsm
