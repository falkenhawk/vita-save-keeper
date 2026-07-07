#pragma once

#include "core/SaveRecord.hpp"

#include <functional>
#include <string>
#include <vector>

namespace vsm::vita {

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
