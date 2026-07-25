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

// One index entry per save: the folder fingerprint, with the save time and the title metadata
// hanging off it. Each half has its own freshness rule - see SaveIndex.
struct SaveIndexEntry {
  SaveFingerprint fingerprint;
  // Tri-state save time. Never resolved (time_resolved false, no savedAt in the file): the save
  // still awaits its first read, which keeps encrypted saves lazy. Resolved with nothing
  // readable (time_resolved true, has_time false, savedAt null): cached so an empty save is not
  // re-mounted every launch just to learn "unknown" again. Known (both true, savedAt string).
  // has_time is never set without time_resolved.
  bool time_resolved{};
  bool has_time{};
  SaveDateTime saved_at;
  // True when the title fields came from the system app database; they stay fresh while the
  // database stamp is unchanged. False for param.sfo-derived titles, which follow the folder
  // fingerprint - the time half always follows the fingerprint.
  bool from_app_db{};
  std::string display_name;
  std::string title_id;
  std::string icon_path;
};

// Merged on-card index (save-titles.json, version 2): times and titles in one file, one
// fingerprint per save instead of the two the old save-times.json/save-titles.json pair kept.
// An accelerator, never a source of truth: a missing, stale, or corrupt file only means times
// are re-read (through a mount where needed) and titles fall back to param.sfo or the app
// database.
struct SaveIndex {
  // Stamp of the system app database the entries were built against; 0/0 when never queried.
  long long app_db_mtime{};
  long long app_db_size{};
  std::map<std::string, SaveIndexEntry> entries;
};

constexpr int kSaveIndexVersion = 2;
constexpr std::size_t kMaxSaveIndexSize = 512 * 1024;

// titleId is omitted for an entry whose title id equals its save id (most of them); the parser
// fills the id back in. An explicitly empty titleId round-trips as empty. fingerprintOk is
// written only when false, so a partial fingerprint stays partial after a round-trip.
std::string serialize_save_index(const SaveIndex &index);
// Accepts version 2 and, read forward, version 1 - the old titles-only save-titles.json: same
// fields, never a savedAt, so every time parses as unresolved and the title half stays warm.
// False (and an empty index) for corrupt input or an unknown version; individual malformed
// entries are skipped so one bad record cannot discard the rest.
bool parse_save_index(const std::string &json, SaveIndex *index);
SaveIndex read_save_index(const std::string &path);
bool write_save_index_atomic(const std::string &path, const SaveIndex &index,
                             std::string *error);

// mtime and byte size of one file; false when it cannot be stat'ed. The app-database stamp and
// similar freshness checks use this.
bool stat_file_stamp(const std::string &path, long long *mtime, long long *size);

} // namespace vsm
