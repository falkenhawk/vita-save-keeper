#pragma once

#include "core/BackupName.hpp"

#include <string>

namespace vsm {

struct BackupRequest {
  std::string source_path;
  std::string backup_root;
  std::string save_id;
  BackupTimestamp timestamp;
};

struct BackupResult {
  bool ok{};
  std::string archive_path;
  std::string error;
};

BackupResult create_backup_archive(const BackupRequest &request);

} // namespace vsm
