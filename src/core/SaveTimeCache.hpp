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

} // namespace vsm
