#pragma once

#include "core/BackupArchive.hpp"
#include "core/BackupName.hpp"

#include <string>
#include <vector>

namespace vsm {

std::vector<std::string> scan_local_backup_names(const std::string &backup_root,
                                                 const std::string &save_id);
std::string local_backup_archive_path(const std::string &backup_root, const std::string &save_id,
                                      const std::string &backup_name);
std::string local_backup_metadata_path(const std::string &backup_root, const std::string &save_id,
                                       const std::string &backup_name);
std::string allocate_backup_name(const BackupTimestamp &timestamp, const std::string &suffix,
                                 const std::vector<std::string> &local_names,
                                 const std::vector<std::string> &remote_names);

struct BackupCreationPlan {
  std::string archive_name;
  bool reuse_existing{};
};

BackupCreationPlan plan_backup_creation(const BackupTimestamp &timestamp,
                                        const std::string &suffix,
                                        const std::vector<ArchiveEntryInfo> &current_entries,
                                        const std::string &backup_root,
                                        const std::string &save_id,
                                        const std::vector<std::string> &local_names,
                                        const std::vector<std::string> &remote_names);

} // namespace vsm
