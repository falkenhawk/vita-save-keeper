#include "core/BackupList.hpp"

namespace vsm {

BackupEntry BackupEntry::new_backup() {
  return {BackupEntryKind::NewBackup, "New Backup"};
}

BackupEntry BackupEntry::local(std::string file_name) {
  return {BackupEntryKind::Local, std::move(file_name)};
}

BackupEntry BackupEntry::remote(std::string file_name) {
  return {BackupEntryKind::Remote, std::move(file_name)};
}

std::string BackupEntry::display_name() const {
  if (kind == BackupEntryKind::Remote) {
    // Match JKSV's visual model: local backups keep their plain timestamp name, while cloud
    // entries get a provider prefix. Keeping the stored file name unmodified avoids leaking UI
    // labels into local paths or Drive object names.
    return "[GD] " + name;
  }
  // Automatic pre-restore snapshots carry an " auto" marker in the file name (so chronological
  // sorting still works) and are displayed with an [AUTO] prefix instead.
  constexpr const char *kAutoSuffix = " auto.zip";
  const std::size_t suffix_length = 9;
  if (kind == BackupEntryKind::Local && name.size() > suffix_length &&
      name.compare(name.size() - suffix_length, suffix_length, kAutoSuffix) == 0) {
    return "[AUTO] " + name.substr(0, name.size() - suffix_length) + ".zip";
  }
  return name;
}

std::vector<BackupEntry> build_backup_menu(const std::vector<std::string> &remote_files,
                                           const std::vector<std::string> &local_files) {
  std::vector<BackupEntry> result;
  result.reserve(1 + remote_files.size() + local_files.size());
  result.push_back(BackupEntry::new_backup());

  // JKSV lists cloud snapshots in the same backup picker instead of forcing a separate sync
  // screen. Remote entries are inserted before local entries so the prefixed Drive backups stay
  // grouped and easy to scan, while local saves remain untagged as requested.
  for (const std::string &file : remote_files) {
    result.push_back(BackupEntry::remote(file));
  }
  for (const std::string &file : local_files) {
    result.push_back(BackupEntry::local(file));
  }

  return result;
}

} // namespace vsm
