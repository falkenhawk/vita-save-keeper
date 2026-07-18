#pragma once

#include "core/SaveRecord.hpp"

#include <functional>
#include <string>
#include <vector>

namespace vsm::vita {

// The system database titles and icons are read from. Its file stamp (mtime+size) doubles as
// the title cache's freshness signal: installs, updates, and deletions rewrite this file.
constexpr const char *kSystemAppDbPath = "ur0:shell/db/app.db";

struct AppDbMetadataResult {
  bool ok{};
  std::string error;
};

// on_progress, if set, is called periodically while the app-database query runs (once every few
// rows) so a caller can keep a startup "loading" animation moving instead of frozen. Row totals
// are not known up front, so this is a pulse, not a percentage.
AppDbMetadataResult apply_app_db_metadata(std::vector<SaveRecord> *saves,
                                          const std::function<void()> &on_progress = {});

} // namespace vsm::vita
