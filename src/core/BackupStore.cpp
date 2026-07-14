#include "core/BackupStore.hpp"

#include "core/BackupName.hpp"
#include "core/PathUtil.hpp"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace vsm {
namespace {

std::string join_path(const std::string &parent, const std::string &child) {
  if (parent.empty()) {
    return child;
  }
  if (parent.back() == '/') {
    return parent + child;
  }
  return parent + "/" + child;
}

bool is_dot_entry(const char *name) {
  return std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0;
}

bool is_regular_file(const std::string &path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool has_zip_extension(const std::string &name) {
  constexpr const char *kZipExtension = ".zip";
  if (name.size() < std::strlen(kZipExtension)) {
    return false;
  }
  return name.compare(name.size() - std::strlen(kZipExtension), std::strlen(kZipExtension),
                      kZipExtension) == 0;
}

std::string normalized_save_folder(const std::string &save_id) {
  std::string save_folder = normalize_path_component(save_id);
  if (save_folder.empty()) {
    save_folder = "unknown-save";
  }
  return save_folder;
}

} // namespace

std::vector<std::string> scan_local_backup_names(const std::string &backup_root,
                                                 const std::string &save_id) {
  std::vector<std::string> backups;
  const std::string backup_directory = join_path(backup_root, normalized_save_folder(save_id));
  DIR *dir = opendir(backup_directory.c_str());
  if (!dir) {
    // Backup folders are created lazily when the first snapshot is made. Missing folders should
    // render as an empty backup list rather than an error state.
    return backups;
  }

  while (dirent *entry = readdir(dir)) {
    if (is_dot_entry(entry->d_name)) {
      continue;
    }

    const std::string name = entry->d_name;
    if (has_zip_extension(name) && is_regular_file(join_path(backup_directory, name))) {
      backups.push_back(name);
    }
  }
  closedir(dir);

  // Timestamped names are zero-padded, so lexical order matches chronological order. Descending
  // order puts the newest restore point at the top of the controller menu.
  std::sort(backups.rbegin(), backups.rend());
  return backups;
}

std::string local_backup_archive_path(const std::string &backup_root, const std::string &save_id,
                                      const std::string &backup_name) {
  return join_path(join_path(backup_root, normalized_save_folder(save_id)), backup_name);
}

std::string local_backup_metadata_path(const std::string &backup_root, const std::string &save_id,
                                       const std::string &backup_name) {
  return join_path(join_path(backup_root, normalized_save_folder(save_id)),
                   backup_metadata_name(backup_name));
}

} // namespace vsm
