#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace vsm {

enum class SaveTimeSource {
  VitaSlot,
  Filesystem,
  BackupClock,
};

struct SaveDateTime {
  int year{};
  int month{};
  int day{};
  int hour{};
  int minute{};
  int second{};
};

struct SaveSlotMetadata {
  unsigned int id{};
  SaveDateTime modified_at;
  std::string title;
  std::string subtitle;
  std::string details;
};

// One backed-up directory and the zip entry prefix it lives under. An empty prefix means entries sit
// at the archive root, which is only valid for a single-path entry (matches the layout of ordinary
// savedata backups). Lives here, the lowest metadata header, so SaveMetadata can record the restore
// mapping of a tracked backup; TrackedFolders.hpp reuses the same type for its config entries.
struct TrackedPath {
  std::string prefix;
  std::string path;
  // A single file rather than a directory. Backed up as "<prefix>/<name>"; restored by replacing
  // exactly that file (never by clearing a directory), which is why the flag is persisted in the
  // config and the sidecar instead of being re-inferred from the card at restore time.
  bool is_file{};
};

struct SaveMetadata {
  SaveDateTime saved_at;
  SaveTimeSource source{SaveTimeSource::BackupClock};
  std::vector<SaveSlotMetadata> slots;
  // Non-empty only for tracked data-folder backups: the prefix->directory mapping the backup was
  // made from, so a later config edit cannot silently redirect the restore. Regular saves leave it
  // empty and their sidecar omits the field entirely.
  std::vector<TrackedPath> tracked_targets;
  // Content triple of the archive this sidecar describes (signature over the entries list, total
  // uncompressed bytes, file count). Optional at schema v2, same additive pattern as
  // savedata_paths: absent for sidecars written before the triple existed, and never required.
  std::string content_signature;
  long long content_bytes{};
  long long file_count{};
  bool content_known{};
  // Archive content format: empty = entries hold the save's on-disk bytes (every archive before
  // this field existed, and all homebrew/PSP/tracked archives); "plain" = a retail save's entries
  // were written decrypted through a PFS mount and must be restored through one. Optional at
  // schema v2 like the triple; absent keeps old sidecars byte-identical.
  std::string content_format;
};

constexpr std::size_t kSdslotHeaderSize = 0x400;
constexpr std::size_t kSdslotRecordSize = 0x400;
constexpr std::size_t kMaxSaveSlots = 256;
constexpr std::size_t kMaxMetadataJsonSize = 512 * 1024;

struct SaveMetadataJsonResult {
  bool ok{};
  std::string archive_identity;
  SaveMetadata metadata;
  std::string error;
  int schema_version{};
};

// Sidecars are written at kSaveMetadataJsonVersion; every version from the minimum up to it is
// still read (version 1 is migrated to correct its UTC handling, see parse_save_metadata_json).
// The redundant approximate member is no longer written and is ignored when older files carry it.
constexpr int kMinSaveMetadataJsonVersion = 1;
constexpr int kSaveMetadataJsonVersion = 2;

SaveMetadata parse_sdslot_data(const std::vector<unsigned char> &data);
bool save_directory_has_pfs_metadata(const std::string &save_path);
bool save_metadata_is_usable(const SaveMetadataJsonResult &metadata,
                             const std::string &expected_identity);
// True only when the time came from save contents: exact Vita slots or the newest save file.
// A backup-clock fallback must not be published as if it were the game's save time.
bool save_metadata_has_observed_time(const SaveMetadata &metadata);
// The default caps the file walk at kMaxSaveWalkEntries (DirWalk.hpp); a capped walk reports
// BackupClock rather than a partial newest-file time. The explicit-limit overload is for tests.
SaveMetadata resolve_save_metadata(const std::string &save_path,
                                   const SaveDateTime &backup_clock);
SaveMetadata resolve_save_metadata(const std::string &save_path,
                                   const SaveDateTime &backup_clock, long long max_entries);
SaveDateTime current_local_datetime();
std::string format_save_datetime(const SaveDateTime &value);
// Strict "YYYY-MM-DDTHH:MM:SS" with calendar validation; the format format_save_datetime writes.
bool parse_save_datetime(const std::string &text, SaveDateTime *value);
long long save_datetime_to_local_epoch(const SaveDateTime &value);
std::string serialize_save_metadata_json(const std::string &identity,
                                         const SaveMetadata &metadata);
SaveMetadataJsonResult parse_save_metadata_json(const std::string &json);
SaveMetadataJsonResult read_save_metadata_json(const std::string &path);
bool write_save_metadata_json_atomic(const std::string &path,
                                     const std::string &identity,
                                     const SaveMetadata &metadata,
                                     std::string *error);

} // namespace vsm
