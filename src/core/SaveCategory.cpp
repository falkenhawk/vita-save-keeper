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

bool save_id_looks_like_psp(const std::string &save_id) {
  // A PSP save folder is a nine-character game id, optionally followed by a save-name tail
  // (ULUS10041S0, NPEH00130USER1, or the bare NPEH00143).
  if (save_id.size() < 9) {
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i) {
    if (!std::isupper(static_cast<unsigned char>(save_id[i]))) {
      return false;
    }
  }
  for (std::size_t i = 4; i < 9; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(save_id[i]))) {
      return false;
    }
  }
  // The prefix is what separates the platforms, and it does so even for a bare nine-character id
  // that carries no tail. Sony issued PSP ids as UMD discs (UL.. / UC..) and PSN downloads (NP..),
  // while a Vita game is PCS.., a Vita system app is NPXS.., and homebrew picks names like
  // ADRBUBMAN or BHBB00001. Requiring the family is stricter than a tail check alone: a nine-plus
  // character id outside these families is left to the app database rather than assumed PSP.
  if (save_id[0] == 'U') {
    return save_id[1] == 'L' || save_id[1] == 'C';
  }
  if (save_id[0] == 'N' && save_id[1] == 'P') {
    // NPXS is the Vita's own system-app family; every other NP.. id is PSP-era content.
    return save_id[2] != 'X';
  }
  return false;
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
