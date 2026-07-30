#pragma once

#include "core/SaveRecord.hpp"

#include <string>

namespace vsm {

enum class SaveCategory {
  VitaGame,
  Homebrew,
  Psp,
};

constexpr int kSaveCategoryCount = 3;

SaveCategory classify_save(const SaveRecord &save);
const char *save_category_label(SaveCategory category);

// True when a save folder name is a PSP game id, with or without a save-name tail: four letters
// then five digits, in one of the families Sony issued for PSP content - UMD discs (UL.., UC..)
// and PSN downloads (NP.., excluding the Vita's own NPXS system apps). A Vita game is PCS.. and
// homebrew picks arbitrary names, so neither collides. Used to place a save whose folder is gone;
// a caller holding one of its backup archives should read that instead, which is definitive.
bool save_id_looks_like_psp(const std::string &save_id);

} // namespace vsm
