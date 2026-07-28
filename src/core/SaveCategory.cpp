#include "core/SaveCategory.hpp"

#include <cctype>
#include <string>

namespace vsm {
namespace {

// Retail Vita content uses title ids like PCSB00411 or PCSE00099: "PCS" plus a region letter and
// five digits. Homebrew picks arbitrary nine-character ids (VITADBDLD, ADRBUBMAN), so the strict
// shape check is what separates the two groups.
bool is_retail_vita_title_id(const std::string &id) {
  if (id.size() != 9 || id.compare(0, 3, "PCS") != 0) {
    return false;
  }
  if (!std::isupper(static_cast<unsigned char>(id[3]))) {
    return false;
  }
  for (std::size_t i = 4; i < 9; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(id[i]))) {
      return false;
    }
  }
  return true;
}

} // namespace

// Exploit saves live inside a real retail title's savedata - a retail-shaped id is the very
// mechanism - but their content is homebrew tooling, and that is the tab where users look for
// them. PCSG90096 is h-encore (and h-encore2), riding the "bitter smile" demo.
bool is_exploit_save_id(const std::string &id) { return id == "PCSG90096"; }

SaveCategory classify_save(const SaveRecord &save) {
  if (save.platform == SavePlatform::Psp) {
    return SaveCategory::Psp;
  }
  // Game-card saves always belong to a retail game even when the cartridge title id is unknown.
  if (save.platform == SavePlatform::GameCard) {
    return SaveCategory::VitaGame;
  }
  const std::string &id = save.title_id.empty() ? save.id : save.title_id;
  if (is_exploit_save_id(id)) {
    return SaveCategory::Homebrew;
  }
  return is_retail_vita_title_id(id) ? SaveCategory::VitaGame : SaveCategory::Homebrew;
}

const char *save_category_label(SaveCategory category) {
  switch (category) {
  case SaveCategory::VitaGame:
    return "Vita";
  case SaveCategory::Homebrew:
    return "Homebrew";
  case SaveCategory::Psp:
    return "PSP";
  default:
    return "Saves";
  }
}

} // namespace vsm
