#pragma once

#include "core/BackupName.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vsm {

struct BackupRequest {
  std::string source_path;
  std::string backup_root;
  std::string save_id;
  BackupTimestamp timestamp;
  // Appended to the timestamp in the file name (before ".zip"); automatic pre-restore snapshots
  // use " auto" so they are recognizable and sort next to their timestamp.
  std::string name_suffix;
};

struct BackupResult {
  bool ok{};
  std::string archive_path;
  std::string error;
};

struct RestoreRequest {
  std::string archive_path;
  std::string destination_path;
};

struct RestoreResult {
  bool ok{};
  std::string error;
};

struct ArchiveEntryInfo {
  std::string path;
  std::uint32_t crc32{};
  std::uint32_t size{};
};

BackupResult create_backup_archive(const BackupRequest &request);
RestoreResult restore_backup_archive(const RestoreRequest &request);
// Content signature of a live save folder: relative path, CRC32, and size per file, the same
// values our ZIP writer stores. Used to detect whether a folder is already backed up.
std::vector<ArchiveEntryInfo> compute_folder_entries(const std::string &folder_path, bool *ok);
// True when the archive's central directory lists exactly the given entries.
bool entries_match_backup_archive(const std::vector<ArchiveEntryInfo> &folder_entries,
                                  const std::string &archive_path);

} // namespace vsm
