#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vsm {

// Where PARAM.SFO sits inside an EBOOT.PBP, read from the PBP's fixed header: the SFO is the
// first payload file, so its span runs from its own offset to the next file's offset.
struct PbpSfoSpan {
  bool ok{};
  std::uint32_t offset{};
  std::uint32_t size{};
};

PbpSfoSpan pbp_param_sfo_span(const std::vector<unsigned char> &header);

// Disc ids recorded in Adrenaline's isocache2.bin - its own parse of every ISO and CSO its XMB
// has listed. Unique, in file order; records for deleted files are zeroed by Adrenaline and skip.
std::vector<std::string> isocache_disc_ids(const std::vector<unsigned char> &blob);

// True when one of the installed game ids owns this save folder. A PSP save folder is the game
// code followed by a save-name tail, so ownership is a prefix match on the full 9-char code.
bool psp_save_matches_installed_game(const std::string &save_id,
                                     const std::vector<std::string> &game_ids);

// Every game id installed under the given memory-stick roots: DISC_IDs from PSP/GAME eboots plus
// Adrenaline's isocache. A positive-only signal - ISO files Adrenaline has never listed are
// invisible here, so absence from this list must never hide anything.
std::vector<std::string> scan_installed_psp_game_ids(const std::vector<std::string> &ms_roots);

} // namespace vsm
