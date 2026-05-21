#pragma once

#include <string>
#include <utility>
#include <vector>

namespace vsm {

enum class BackupEntryKind {
  NewBackup,
  Local,
  Remote,
};

struct BackupEntry {
  BackupEntryKind kind{};
  std::string name;

  static BackupEntry new_backup();
  static BackupEntry local(std::string file_name);
  static BackupEntry remote(std::string file_name);

  std::string display_name() const;
};

std::vector<BackupEntry> build_backup_menu(const std::vector<std::string> &remote_files,
                                           const std::vector<std::string> &local_files);

} // namespace vsm
