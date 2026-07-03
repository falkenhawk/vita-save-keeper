#pragma once

#include "core/SaveRecord.hpp"

namespace vsm {

enum class SaveCategory {
  VitaGame,
  Homebrew,
  Psp,
};

constexpr int kSaveCategoryCount = 3;

SaveCategory classify_save(const SaveRecord &save);
const char *save_category_label(SaveCategory category);

} // namespace vsm
