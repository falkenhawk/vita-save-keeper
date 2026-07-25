#pragma once

#include "core/SaveScanner.hpp"

#include <string>

namespace vsm {

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
};

// settings.txt is plain key=value lines; unknown keys are ignored so older builds can read
// files written by newer ones.
AppSettings parse_app_settings(const std::string &text);
std::string serialize_app_settings(const AppSettings &settings);

} // namespace vsm
