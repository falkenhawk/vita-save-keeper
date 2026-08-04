#include "core/PspGameScan.hpp"

#include "core/PathUtil.hpp"
#include "core/SfoParser.hpp"

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <unordered_set>

namespace vsm {
namespace {

// Adrenaline's VirtualPbp cache record (vshctrl/virtualpbpmgr.h): u32 header[10], then
// isofile[256], discid[10], 6 pad bytes, sfotitle[64], 14 ints of file spans, and an
// 8-field ScePspDateTime. The offsets below are that layout; the struct is written to
// isocache2.bin verbatim, so a size change there would be a new cache filename, not a
// silent relayout - vshctrl already bumped it once ("isocache2").
constexpr std::size_t kIsocacheRecordSize = 448;
constexpr std::size_t kIsocacheIsoFileOffset = 40;
constexpr std::size_t kIsocacheDiscIdOffset = 296;
constexpr std::size_t kIsocacheDiscIdSize = 10;

// An EBOOT.PBP's SFO is a few hundred bytes; a span past this is a corrupt header, not a big SFO.
constexpr std::uint32_t kMaxPbpSfoSize = 256 * 1024;

// isocache2.bin is 128 records (57 KB) in stock Adrenaline. The cap only bounds a corrupt or
// foreign file; a legitimate cache from a fork with a raised MAX_FILES still fits well inside.
constexpr std::size_t kMaxIsocacheBytes = 1024 * 1024;

std::uint32_t read_u32le(const std::vector<unsigned char> &data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

// Game codes are uppercase letters and digits (ULES00151, NPEZ00104). Anything else in a discid
// field means the record is stale or corrupt and cannot match a save folder anyway.
bool is_plausible_game_id(const std::string &id) {
  if (id.size() < 9) {
    return false;
  }
  for (const char ch : id) {
    if (!(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9')) {
      return false;
    }
  }
  return true;
}

std::vector<unsigned char> read_file_prefix(const std::string &path, std::size_t max_bytes) {
  std::vector<unsigned char> data;
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return data;
  }
  data.resize(max_bytes);
  data.resize(std::fread(data.data(), 1, data.size(), file));
  std::fclose(file);
  return data;
}

std::vector<unsigned char> read_file_span(const std::string &path, std::uint32_t offset,
                                          std::uint32_t size) {
  std::vector<unsigned char> data;
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return data;
  }
  if (std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0) {
    data.resize(size);
    data.resize(std::fread(data.data(), 1, data.size(), file));
  }
  std::fclose(file);
  return data;
}

std::vector<std::string> list_child_names(const std::string &root_path) {
  std::vector<std::string> names;
  DIR *dir = opendir(root_path.c_str());
  if (!dir) {
    // Most of the five roots do not exist on any given console; an absent root is an empty one.
    return names;
  }
  while (dirent *entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") {
      names.push_back(name);
    }
  }
  closedir(dir);
  std::sort(names.begin(), names.end());
  return names;
}

void add_unique(std::vector<std::string> *ids, std::unordered_set<std::string> *seen,
                std::string id) {
  if (is_plausible_game_id(id) && seen->insert(id).second) {
    ids->push_back(std::move(id));
  }
}

} // namespace

PbpSfoSpan pbp_param_sfo_span(const std::vector<unsigned char> &header) {
  PbpSfoSpan span;
  // Magic "\0PBP", then version, then eight file offsets; PARAM.SFO's is first, ICON0.PNG's
  // second. 16 bytes cover everything needed even though the full header is 40.
  if (header.size() < 16 || header[0] != 0x00 || header[1] != 'P' || header[2] != 'B' ||
      header[3] != 'P') {
    return span;
  }
  const std::uint32_t sfo_offset = read_u32le(header, 8);
  const std::uint32_t icon0_offset = read_u32le(header, 12);
  // The SFO begins right after the 40-byte header; an offset before that, a next-file offset that
  // does not follow it, or an outsized span all mean the header is not a real PBP's.
  if (sfo_offset < 40 || icon0_offset <= sfo_offset ||
      icon0_offset - sfo_offset > kMaxPbpSfoSize) {
    return span;
  }
  span.ok = true;
  span.offset = sfo_offset;
  span.size = icon0_offset - sfo_offset;
  return span;
}

std::vector<std::string> isocache_disc_ids(const std::vector<unsigned char> &blob) {
  std::vector<std::string> ids;
  std::unordered_set<std::string> seen;
  for (std::size_t base = 0; base + kIsocacheRecordSize <= blob.size();
       base += kIsocacheRecordSize) {
    // Adrenaline blanks a freed slot by zeroing it; an empty isofile is that blank, whatever the
    // rest of the record still holds.
    if (blob[base + kIsocacheIsoFileOffset] == 0) {
      continue;
    }
    std::string id;
    for (std::size_t i = 0; i < kIsocacheDiscIdSize; ++i) {
      const unsigned char ch = blob[base + kIsocacheDiscIdOffset + i];
      if (ch == 0) {
        break;
      }
      id.push_back(static_cast<char>(ch));
    }
    add_unique(&ids, &seen, std::move(id));
  }
  return ids;
}

bool psp_save_matches_installed_game(const std::string &save_id,
                                     const std::vector<std::string> &game_ids) {
  for (const std::string &game_id : game_ids) {
    // Shorter-than-code ids cannot own a save folder; guarded here too so a caller-built list
    // follows the same rule as the scanners.
    if (game_id.size() < 9 || save_id.size() < game_id.size()) {
      continue;
    }
    if (save_id.compare(0, game_id.size(), game_id) == 0) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> scan_installed_psp_game_ids(const std::vector<std::string> &ms_roots) {
  std::vector<std::string> ids;
  std::unordered_set<std::string> seen;
  for (const std::string &root : ms_roots) {
    const std::string game_root = join_path(root, "PSP/GAME");
    for (const std::string &child : list_child_names(game_root)) {
      const std::string pbp_path = join_path(join_path(game_root, child), "EBOOT.PBP");
      const PbpSfoSpan span = pbp_param_sfo_span(read_file_prefix(pbp_path, 16));
      if (span.ok) {
        const SfoMetadata metadata =
            parse_sfo_metadata(read_file_span(pbp_path, span.offset, span.size));
        const auto disc_id = metadata.strings.find("DISC_ID");
        if (disc_id != metadata.strings.end()) {
          add_unique(&ids, &seen, disc_id->second);
        }
      }
      // The folder name doubles as an id: PSN installs are named by their game code, and it is
      // all there is when the eboot is corrupt or a homebrew SFO carries no DISC_ID.
      add_unique(&ids, &seen, child);
    }
    for (std::string &id :
         isocache_disc_ids(read_file_prefix(join_path(root, "PSP/SYSTEM/isocache2.bin"),
                                            kMaxIsocacheBytes))) {
      add_unique(&ids, &seen, std::move(id));
    }
  }
  return ids;
}

} // namespace vsm
