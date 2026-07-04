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
  std::string title_id;
  std::string display_name;
  std::string path;
  std::string icon_path;
  // Newest modification time inside the save folder (shallow); proxy for "last played".
  long long saved_at_epoch{};
};

} // namespace vsm
