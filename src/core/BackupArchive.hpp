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

struct RestoreRequest {
  std::string archive_path;
  std::string destination_path;
};

struct RestoreResult {
  bool ok{};
  std::string error;
};

BackupResult create_backup_archive(const BackupRequest &request);
RestoreResult restore_backup_archive(const RestoreRequest &request);

} // namespace vsm
