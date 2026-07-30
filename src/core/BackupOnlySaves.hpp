#pragma once

#include <string>
#include <vector>

namespace vsm {

// Save keys for Drive folders no known save covers: candidates for a backups-only restore row.
// folder_names are the Drive index keys ("<key>" as written by 1.0, or "<key> <title>" since);
// known_ids are the ids of every record already in the grid, matched the same way
// resolved_drive_folder_name matches them (normalize_path_component + drive_folder_matches_save),
// so both folder forms of a known save suppress its candidate. The returned key is the folder
// name's leading token, deduplicated, in folder order. A hypothetical id containing a space
// would extract short here; the caller's app-database gate then drops it, so a bad parse can
// only ever lose a candidate, never invent a restore destination.
std::vector<std::string> backup_only_save_keys(const std::vector<std::string> &folder_names,
                                              const std::vector<std::string> &known_ids);

} // namespace vsm
