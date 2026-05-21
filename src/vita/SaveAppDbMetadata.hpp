#pragma once

#include "core/SaveRecord.hpp"

#include <string>
#include <vector>

namespace vsm::vita {

struct AppDbMetadataResult {
  bool ok{};
  std::string error;
};

AppDbMetadataResult apply_app_db_metadata(std::vector<SaveRecord> *saves);

} // namespace vsm::vita
