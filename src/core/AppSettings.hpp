#pragma once

#include "core/SaveScanner.hpp"

#include <string>

namespace vsm {

// Provisional until the on-device benchmark picks the shipped default (see the compression spec).
constexpr int kDefaultBackupCompressionLevel = 6;

struct AppSettings {
  SaveSortMode sort_mode{SaveSortMode::Name};
  // Set once the startup sweep has removed backup folders left empty by older versions. Absent in
  // files written before this key existed, which is exactly what makes those installs sweep once.
  // Anyone rebuilding an AppSettings before saving must copy this flag forward (see
  // App::save_settings), or a settings change would silently re-arm the sweep.
  bool cleaned_empty_backup_folders{};
  // The backup-settings.json "modified" stamp both sides agreed on at the last successful Drive
  // sync. Device-local by design - it describes THIS device's sync state, so it lives here and
  // never in the synced file itself.
  long long backup_settings_synced{};
  // zlib level for new backup archives: 0 = store-only, 1-9 = deflate. Device-local because it
  // trades this device's CPU time for space. The default is not serialized, so installs that
  // never overrode it follow the shipped default when it changes.
  int backup_compression_level{kDefaultBackupCompressionLevel};
};

// settings.txt is plain key=value lines; unknown keys are ignored so older builds can read
// files written by newer ones.
AppSettings parse_app_settings(const std::string &text);
std::string serialize_app_settings(const AppSettings &settings);

} // namespace vsm
