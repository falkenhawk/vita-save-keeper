#pragma once

#include <string>

namespace vsm {

enum class SavePlatform {
  Vita,
  GameCard,
  Psp,
};

struct SaveRecord {
  SavePlatform platform{};
  std::string id;
  std::string display_name;
  std::string path;
  std::string icon_path;
};

} // namespace vsm
