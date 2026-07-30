#include "core/BackupOnlySaves.hpp"

#include "core/GoogleDrive.hpp"
#include "core/PathUtil.hpp"

#include <unordered_set>

namespace vsm {

std::vector<std::string> backup_only_save_keys(const std::vector<std::string> &folder_names,
                                              const std::vector<std::string> &known_ids) {
  // Normalized once here; the ids arrive raw from the records. This is the id->folder direction
  // resolved_drive_folder_name uses, so coverage below agrees with how the rest of the app pairs
  // folders with saves.
  std::vector<std::string> known_keys;
  known_keys.reserve(known_ids.size());
  for (const std::string &id : known_ids) {
    std::string key = normalize_path_component(id);
    if (!key.empty()) {
      known_keys.push_back(std::move(key));
    }
  }

  std::vector<std::string> keys;
  std::unordered_set<std::string> seen;
  for (const std::string &folder : folder_names) {
    bool covered = false;
    for (const std::string &key : known_keys) {
      if (drive_folder_matches_save(folder, key)) {
        covered = true;
        break;
      }
    }
    if (covered) {
      continue;
    }
    const std::size_t space = folder.find(' ');
    std::string key = space == std::string::npos ? folder : folder.substr(0, space);
    if (key.empty() || !seen.insert(key).second) {
      continue;
    }
    keys.push_back(std::move(key));
  }
  return keys;
}

} // namespace vsm
