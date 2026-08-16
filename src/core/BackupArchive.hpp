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

// One small, wholly in-memory file to fold into an archive as its own entry (Amendment B's raw
// PFS skeleton): the App reads these into memory itself - see BackupRequest::raw_entries - because
// they must be captured UNMOUNTED, before the held mount the rest of a plain archive is written
// through is even acquired (a mounted view cannot supply raw bytes: sce_pfs is invisible through
// it, and sce_sys shows only decrypted content). The writer just stores or deflates whatever bytes
// it is handed under zip_path; it has no opinion on where they came from.
struct InMemoryEntry {
  std::string zip_path;
  std::vector<unsigned char> data;
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
  // Polled per chunk through the hash and write passes; returning true aborts the archive and the
  // partial file is removed like any other failure. The caller knows the abort was its own, so it
  // can tell "canceled" from a real error.
  std::function<bool()> cancel_check;
  // zlib level for entry data: 0 stores every entry (the pre-compression archive shape), 1-9
  // deflates each file, falling back to store per file whenever deflate does not shrink it.
  // Every reader accepts both shapes, and the central directory always carries the uncompressed
  // CRC32 and size, so change detection is identical either way.
  int compression_level{};
  // Writes the plain-content format/compatibility entry as the archive's first entry
  // (kPlainFormatEntryName, ".savekeeper" at the archive root - Amendment B). Set only by the
  // App's plain backup path together with compression_level >= 1; the entry's prose content
  // always deflates, so app versions that predate deflate support (which reject any method-8
  // entry before touching a live save) refuse the whole archive instead of restoring decrypted
  // bytes as if they were raw.
  bool add_plain_format_entry{};
  // Raw (encrypted, unmounted) sce_sys/sce_pfs skeleton files, pre-read into memory by the App -
  // see InMemoryEntry's own doc. Written right after the format entry, before any game-file
  // entries, under kRawSkeletonPrefix zip paths. Only ever set together with
  // add_plain_format_entry; empty for every other kind of archive.
  std::vector<InMemoryEntry> raw_entries;
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
  // Sum of the uncompressed sizes of every entry extract_backup_archive_for_inspection actually
  // extracted (the plain-content marker excluded, same as everywhere else it is invisible). Only
  // that call fills this in; restore_backup_archive's own extraction has no caller that needs it,
  // so it is left at 0 there. A plain restore's progress bar uses this as its real content-byte
  // denominator: the archive's own byte count (extract_archive_to_directory's "progress" callback)
  // is compressed-archive bytes, not the decrypted content a held-mount copy actually moves, and
  // this total is already computed as a side effect of extraction - no extra directory walk needed.
  std::uint64_t content_bytes{};
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

// Marker entry recognized only for restoring plain-content archives written before Amendment B's
// raw PFS skeleton existed (none shipped - this session's own test archives only): never written
// by this version, kept purely so restore can still dispatch such an archive to the fallback
// three-tier restore (restore_plain_content_archive_fallback) instead of refusing it outright. Old
// app versions that predate deflate support reject the whole archive because this entry was always
// deflate-compressed.
constexpr const char *kPlainContentMarkerName = ".save-keeper-plain";

// Amendment B's plain-content format/compatibility entry, at the archive root (NOT under
// kRawSkeletonPrefix): every plain-content archive this app writes from here on carries exactly
// this entry as its first, in place of the old kPlainContentMarkerName. Three jobs: (1) restore's
// format-dispatch signal (two-phase raw-skeleton restore vs. the old three-tier fallback), (2) the
// same old-version tripwire the marker used to serve alone - always deflate-compressed, so an app
// version predating deflate support refuses the whole archive before touching a live save, and
// (3) self-documentation for a PC user who opens the archive directly. Its content's first line
// ends with a decimal format version restore reads with parse_plain_format_version.
constexpr const char *kPlainFormatEntryName = ".savekeeper";

// Zip-path prefix for the raw (encrypted, unmounted) sce_sys/sce_pfs skeleton entries a
// plain-content archive carries alongside its decrypted game files (Amendment B): a mutually
// consistent PFS pair captured before the backup's held mount is even acquired, and written back
// raw, before any mount, as the first step of a two-phase restore. Excluded from signatures, entry
// comparisons, and details counts/sizes (see ContentSignature.hpp's comparison_entries) - it is
// restore bookkeeping, not savedata content the user or the change-detection check cares about.
constexpr const char *kRawSkeletonPrefix = ".raw/";

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

// Recursively deletes a directory and everything in it, the folder itself included; a path that
// does not exist counts as deleted. Refuses the unsafe roots the restore destination checks
// refuse. The live-savedata delete uses this; restore keeps clearing contents only, because it
// re-fills the same folder.
bool remove_directory_tree(const std::string &path);
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
// because hashing a large save takes long enough to freeze a single busy frame. cancel_check,
// when set, is polled per read chunk; returning true aborts the walk and reports *ok false -
// the caller knows the abort was its own, so it can tell "canceled" from "unreadable".
std::vector<ArchiveEntryInfo> compute_folder_entries(
    const std::string &folder_path, bool *ok,
    const std::function<void(std::uint64_t done, std::uint64_t total)> &progress = {},
    const std::function<bool()> &cancel_check = {});
// Same content signature, but for several sources (directories or single files) bundled under
// per-source prefixes (see BackupRequest::sources). A missing source is skipped, not a failure;
// *ok is true when every existing source walked cleanly (all sources missing yields an empty
// result and *ok true - the "nothing to back up" decision belongs to the caller). cancel_check
// as in compute_folder_entries.
std::vector<ArchiveEntryInfo> compute_sources_entries(
    const std::vector<BackupSource> &sources, bool *ok,
    const std::function<bool()> &cancel_check = {});
// Entry list (relative path, CRC32, uncompressed size) from an archive's central directory,
// read without decompressing anything. False when the file is not one of our readable ZIPs.
bool read_archive_central_directory(const std::string &archive_path,
                                    std::vector<ArchiveEntryInfo> *out);
// True iff the archive's central directory lists an entry named kPlainContentMarkerName - it was
// written decrypted through a held PFS mount (Task A2) and must be restored through one too.
// *cd_ok (when non-null) reports whether the central directory could be read at all. A caller
// that must not guess when it could not (e.g. handle_restore's dispatch, which must never fall
// back to the raw rename-based restore and risk writing plaintext into an encrypted, unmounted
// save just because only the archive's trailing central directory - not its local headers - is
// damaged) checks *cd_ok before trusting the return value; a caller with its own separate
// central-directory read and fallback can omit the parameter and treat "unreadable" as "false"
// exactly as before this parameter existed.
bool archive_has_plain_marker(const std::string &archive_path, bool *cd_ok = nullptr);
// Same contract as archive_has_plain_marker, but for kPlainFormatEntryName (".savekeeper") -
// Amendment B's replacement, written by every plain-content archive this app creates from here on
// and restore's signal to use the two-phase raw-skeleton restore rather than the old fallback.
bool archive_has_plain_format_entry(const std::string &archive_path, bool *cd_ok = nullptr);
// Parses the format version from a kPlainFormatEntryName entry's content: the trailing decimal
// integer on its FIRST LINE (our own writer's first line always ends "... - format 1", see
// BackupArchive.cpp's kFormatEntryContent). Reads backward from the end of that line while it
// sees ASCII digits; no trailing digits there, or empty content, defaults to 1 - our own writer
// always emits this exact shape, so anything else only happens via a hand-edited or corrupted
// entry outside our control, and treating that permissively (rather than refusing an archive our
// own writer produced) is the safer default. Exported so restore dispatch (App-side, not host
// testable on its own) can be pinned by a host test against this parser directly.
int parse_plain_format_version(const std::string &content);
// True when the archive's central directory lists exactly the given entries, once both sides are
// run through ContentSignature.hpp's comparison_entries (sce_pfs excluded from both, so an old raw
// archive that still stores it stays matchable against today's filtered folder walk).
bool entries_match_backup_archive(const std::vector<ArchiveEntryInfo> &folder_entries,
                                  const std::string &archive_path);
// Reads one entry - store or deflate - bounded by max_size.
ArchiveReadResult read_backup_entry(const std::string &archive_path, const std::string &entry_path,
                                    std::size_t max_size);

// On-demand sizes for the details view; both cheap (stat sums, no PFS mount).
// compute_folder_size is the total bytes of every regular file under a live save folder, with an
// optional count of those files; archive_file_size is a backup ZIP's size on disk. Each sets *ok
// to false when unreadable.
std::uint64_t compute_folder_size(const std::string &folder_path, bool *ok,
                                  std::size_t *file_count = nullptr);
std::uint64_t archive_file_size(const std::string &archive_path, bool *ok);

} // namespace vsm
