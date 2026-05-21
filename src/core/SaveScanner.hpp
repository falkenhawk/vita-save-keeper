#pragma once

#include "core/SaveRecord.hpp"

#include <string>
#include <vector>

namespace vsm {

struct SaveRoot {
  SavePlatform platform{};
  std::string path;
};

std::vector<SaveRecord> scan_save_roots(const std::vector<SaveRoot> &roots);

} // namespace vsm
