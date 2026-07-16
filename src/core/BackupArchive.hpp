#pragma once

#include "core/BackupName.hpp"

#include <cstdint>
#include <functional>
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
  // Exact collision-safe basename allocated by the caller. Empty retains timestamp/suffix naming.
  std::string archive_name;
  // Optional: called with (bytes written, total bytes) as the archive is written, throttled, so a
  // caller can animate a progress bar for a large save. Empty by default (the batch leaves it
  // unset, so only the single "Creating backup" modal fills).
  std::function<void(std::uint64_t done, std::uint64_t total)> progress;
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
  // Inspection uses this to recognize legacy Save Keeper archives, which stamped every entry
  // with one synthetic backup time. Such a timestamp must not be presented as a file save time.
  bool file_timestamps_uniform{};
};

struct ArchiveEntryInfo {
  std::string path;
  std::uint32_t crc32{};
  std::uint32_t size{};
};

struct ArchiveReadResult {
  bool ok{};
  std::vector<unsigned char> data;
  std::string error;

  // A missing optional file is different from a damaged ZIP: callers can stop cleanly instead
  // of extracting the whole archive in an attempt to decrypt a file that was never there.
  bool entry_missing() const { return !ok && error == "entry not found"; }
};

BackupResult create_backup_archive(const BackupRequest &request);
RestoreResult restore_backup_archive(const RestoreRequest &request);
// Extracts into a brand-new work directory without touching a live save. Metadata inspection uses
// this before asking the Vita to mount an encrypted backup copy.
RestoreResult extract_backup_archive_for_inspection(const std::string &archive_path,
                                                     const std::string &destination_path,
                                                     std::uint64_t max_total_bytes =
                                                         512ULL * 1024ULL * 1024ULL);
// Removes only an isolated inspection directory; empty paths and filesystem roots are rejected.
bool remove_backup_inspection_directory(const std::string &path);
// Content signature of a live save folder: relative path, CRC32, and size per file, the same
// values our ZIP writer stores. Used to detect whether a folder is already backed up.
std::vector<ArchiveEntryInfo> compute_folder_entries(const std::string &folder_path, bool *ok);
// True when the archive's central directory lists exactly the given entries.
bool entries_match_backup_archive(const std::vector<ArchiveEntryInfo> &folder_entries,
                                  const std::string &archive_path);
ArchiveReadResult read_stored_backup_entry(const std::string &archive_path,
                                           const std::string &entry_path,
                                           std::size_t max_size);

} // namespace vsm
