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

struct SaveMetadata {
  SaveDateTime saved_at;
  SaveTimeSource source{SaveTimeSource::BackupClock};
  bool approximate{true};
  std::vector<SaveSlotMetadata> slots;
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
constexpr int kMinSaveMetadataJsonVersion = 1;
constexpr int kSaveMetadataJsonVersion = 2;

SaveMetadata parse_sdslot_data(const std::vector<unsigned char> &data);
bool save_directory_has_pfs_metadata(const std::string &save_path);
bool save_metadata_is_usable(const SaveMetadataJsonResult &metadata,
                             const std::string &expected_identity);
// True only when the time came from save contents: exact Vita slots or the newest save file.
// A backup-clock fallback must not be published as if it were the game's save time.
bool save_metadata_has_observed_time(const SaveMetadata &metadata);
SaveMetadata resolve_save_metadata(const std::string &save_path,
                                   const SaveDateTime &backup_clock);
SaveDateTime current_local_datetime();
std::string format_save_datetime(const SaveDateTime &value);
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
