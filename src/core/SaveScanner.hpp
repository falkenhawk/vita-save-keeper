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
void sort_saves_by_display_name(std::vector<SaveRecord> *saves);

} // namespace vsm
