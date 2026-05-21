#pragma once

#include <string>
#include <vector>

namespace vsm {

std::vector<std::string> scan_local_backup_names(const std::string &backup_root,
                                                 const std::string &save_id);

} // namespace vsm
