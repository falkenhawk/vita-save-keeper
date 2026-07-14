#pragma once

#include <string>
#include <vector>

namespace vsm {

std::vector<std::string> scan_local_backup_names(const std::string &backup_root,
                                                 const std::string &save_id);
std::string local_backup_archive_path(const std::string &backup_root, const std::string &save_id,
                                      const std::string &backup_name);
std::string local_backup_metadata_path(const std::string &backup_root, const std::string &save_id,
                                       const std::string &backup_name);

} // namespace vsm
