#pragma once

#include "core/SaveSlotMetadata.hpp"

#include <cstddef>
#include <map>
#include <string>

namespace vsm {

// Content fingerprint of a save folder, computed from stat() alone - no mount, no decryption.
// A game save rewrites files, so a real change moves at least one of these values; all three
// together make a missed change practically impossible (it would need identical file count and
// byte total plus mtimes inside the filesystem's timestamp granularity).
struct SaveFingerprint {
  bool ok{};
  long long newest_mtime{};
  long long file_count{};
  long long total_bytes{};

  bool matches(const SaveFingerprint &other) const {
    return ok && other.ok && newest_mtime == other.newest_mtime &&
           file_count == other.file_count && total_bytes == other.total_bytes;
  }
};

SaveFingerprint compute_save_fingerprint(const std::string &save_path);

// Cache of mount-resolved save times, keyed by save id. An entry is trusted only while the save
// folder still matches its fingerprint, so playing a game (or restoring a backup) invalidates it
// naturally. The cache is an accelerator, never a source of truth: a missing, stale, or corrupt
// file only means those times are read through a mount again.
struct SaveTimeCacheEntry {
  SaveFingerprint fingerprint;
  // False for a save that resolved to no readable time at all (an empty save, or one whose slot
  // table and files gave nothing). Cached so such a game is not re-mounted every launch just to
  // learn "unknown" again; any folder change still invalidates the entry.
  bool has_time{};
  SaveDateTime saved_at;
};

struct SaveTimeCache {
  std::map<std::string, SaveTimeCacheEntry> entries;
};

constexpr int kSaveTimeCacheVersion = 1;
constexpr std::size_t kMaxSaveTimeCacheSize = 512 * 1024;

std::string serialize_save_time_cache(const SaveTimeCache &cache);
// False (and an empty cache) for corrupt input or an unknown version; individual malformed
// entries are skipped so one bad record cannot discard the rest.
bool parse_save_time_cache(const std::string &json, SaveTimeCache *cache);
SaveTimeCache read_save_time_cache(const std::string &path);
bool write_save_time_cache_atomic(const std::string &path, const SaveTimeCache &cache,
                                  std::string *error);

// Cache of each save's resolved title metadata, so a warm scan skips the param.sfo reads and the
// system app-database query entirely. Two freshness rules, matched to where a value comes from:
// entries resolved from the app database stay valid while the database file's stamp (mtime+size)
// is unchanged - installs, updates, and deletions rewrite it - and entries resolved from a save's
// own param.sfo stay valid while the save folder's fingerprint matches, since that sfo lives
// inside the folder. Icon content is read from disk every session; only the path is cached.
struct SaveTitleCacheEntry {
  bool from_app_db{};
  // Freshness key for sfo-derived entries; ignored when from_app_db.
  SaveFingerprint fingerprint;
  std::string display_name;
  std::string title_id;
  std::string icon_path;
};

struct SaveTitleCache {
  // Stamp of the system app database the entries were built against; 0/0 when never queried.
  long long app_db_mtime{};
  long long app_db_size{};
  std::map<std::string, SaveTitleCacheEntry> entries;
};

constexpr int kSaveTitleCacheVersion = 1;

std::string serialize_save_title_cache(const SaveTitleCache &cache);
bool parse_save_title_cache(const std::string &json, SaveTitleCache *cache);
SaveTitleCache read_save_title_cache(const std::string &path);
bool write_save_title_cache_atomic(const std::string &path, const SaveTitleCache &cache,
                                   std::string *error);

// mtime and byte size of one file; false when it cannot be stat'ed. The app-database stamp and
// similar freshness checks use this.
bool stat_file_stamp(const std::string &path, long long *mtime, long long *size);

} // namespace vsm
