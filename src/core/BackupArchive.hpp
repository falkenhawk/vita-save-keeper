#pragma once

#include "core/BackupName.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vsm {

// One directory - or a single file - to fold into a multi-source archive, under its own zip path
// prefix. A file source contributes exactly one entry, "<prefix>/<name>". An empty prefix is only
// valid when it is the sole source (single-directory archives keep flat paths).
struct BackupSource {
  std::string prefix;
  std::string path;
};

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
  // Optional: called with (bytes read, total bytes) across the archive's hash and write passes as
  // one continuous stream, throttled, so a caller can animate a progress bar for a large save.
  std::function<void(std::uint64_t done, std::uint64_t total)> progress;
  // When non-empty, sources wins over source_path: a save spread over several directories (its
  // savedata folder plus extra data folders, e.g. RetroArch's savefiles/ and savestates/) bundles
  // them into one archive, each under its own zip path prefix. A source whose directory is missing
  // contributes nothing, but at least one source directory must exist.
  // Appended last (rather than next to source_path) so existing positional-brace-init callers,
  // which only ever supply source_path..progress, keep compiling unchanged.
  std::vector<BackupSource> sources;
};

struct BackupResult {
  bool ok{};
  std::string archive_path;
  std::string error;
};

// One zip path prefix mapped back to the live directory it should repopulate - or, with is_file,
// the single file it should replace. File targets never clear a directory: the staged entry moves
// over the file, and a prefix absent from the archive removes it (mirroring the backup).
struct RestoreTarget {
  std::string prefix;
  std::string destination_path;
  bool is_file{};
};

struct RestoreRequest {
  std::string archive_path;
  std::string destination_path;
  // Optional: called with (bytes consumed from the archive, archive file size) as entries are
  // extracted, throttled like the writer's callback, so a caller can animate a progress bar for
  // a large save instead of holding one frozen frame through the whole extraction.
  std::function<void(std::uint64_t done, std::uint64_t total)> progress;
  // When non-empty, targets wins over destination_path: each entry maps one zip path prefix back
  // to its own directory, which is cleared and repopulated independently (mirrors
  // BackupRequest::sources so a multi-source archive restores to the right places).
  // Restore is per-directory, not transactional: targets are cleared and repopulated in the
  // given order, so a failure on a later target leaves earlier targets already restored and the
  // failing target cleared. The archive itself is untouched, so retrying the restore is the
  // recovery.
  // Every destination must live on the same filesystem as the archive, since moving staged files
  // into place is a rename. On device both are under ux0, and tracked folders are only ever
  // created inside ux0:data.
  std::vector<RestoreTarget> targets;
};

struct RestoreResult {
  bool ok{};
  std::string error;
  // Inspection uses this to recognize legacy Save Keeper archives, which stamped every entry
  // with one synthetic backup time. Such a timestamp must not be presented as a file save time.
  bool file_timestamps_uniform{};
};

// Shared invariant of BackupRequest::sources and RestoreRequest::targets (and the sidecar targets a
// tracked restore reads back through tracked_targets_are_safe): the empty prefix means "no prefix",
// which only makes sense when there is exactly one entry. Once a request bundles more than one path,
// every prefix must be present and distinct, or the archive could not map each entry back to the
// right directory. Templated because BackupSource, RestoreTarget, and TrackedPath all carry .prefix.
template <typename PrefixedPath>
bool tracked_paths_are_well_formed(const std::vector<PrefixedPath> &paths) {
  if (paths.size() > 1) {
    for (const PrefixedPath &path : paths) {
      if (path.prefix.empty()) {
        return false;
      }
    }
  }
  for (std::size_t i = 0; i < paths.size(); ++i) {
    for (std::size_t j = i + 1; j < paths.size(); ++j) {
      if (paths[i].prefix == paths[j].prefix) {
        return false;
      }
    }
  }
  return true;
}

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
RestoreResult extract_backup_archive_for_inspection(
    const std::string &archive_path, const std::string &destination_path,
    std::uint64_t max_total_bytes = 512ULL * 1024ULL * 1024ULL,
    const std::function<void(std::uint64_t done, std::uint64_t total)> &progress = {});
// Removes only an isolated inspection directory; empty paths and filesystem roots are rejected.
bool remove_backup_inspection_directory(const std::string &path);
// Content signature of a live save folder: relative path, CRC32, and size per file, the same
// values our ZIP writer stores. Used to detect whether a folder is already backed up. The
// optional callback reports (bytes hashed, total bytes) while the folder is read, throttled,
// because hashing a large save takes long enough to freeze a single busy frame.
std::vector<ArchiveEntryInfo> compute_folder_entries(
    const std::string &folder_path, bool *ok,
    const std::function<void(std::uint64_t done, std::uint64_t total)> &progress = {});
// Same content signature, but for several sources (directories or single files) bundled under
// per-source prefixes (see BackupRequest::sources). A missing source is skipped, not a failure;
// *ok is true when every existing source walked cleanly (all sources missing yields an empty
// result and *ok true - the "nothing to back up" decision belongs to the caller).
std::vector<ArchiveEntryInfo> compute_sources_entries(const std::vector<BackupSource> &sources,
                                                      bool *ok);
// Entry list (relative path, CRC32, uncompressed size) from an archive's central directory,
// read without decompressing anything. False when the file is not one of our readable ZIPs.
bool read_archive_central_directory(const std::string &archive_path,
                                    std::vector<ArchiveEntryInfo> *out);
// True when the archive's central directory lists exactly the given entries.
bool entries_match_backup_archive(const std::vector<ArchiveEntryInfo> &folder_entries,
                                  const std::string &archive_path);
ArchiveReadResult read_stored_backup_entry(const std::string &archive_path,
                                           const std::string &entry_path,
                                           std::size_t max_size);

// On-demand sizes for the details view; both cheap (stat sums, no PFS mount).
// compute_folder_size is the total bytes of every regular file under a live save folder;
// archive_file_size is a backup ZIP's size on disk. Each sets *ok to false when unreadable.
std::uint64_t compute_folder_size(const std::string &folder_path, bool *ok);
std::uint64_t archive_file_size(const std::string &archive_path, bool *ok);

} // namespace vsm
