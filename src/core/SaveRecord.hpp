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
  // Resolved from the newest valid Vita slot, falling back to the newest recursively scanned
  // regular file for formats without slot metadata.
  long long saved_at_epoch{};
};

} // namespace vsm
