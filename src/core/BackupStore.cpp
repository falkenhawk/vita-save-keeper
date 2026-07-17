#include "core/BackupStore.hpp"

#include "core/BackupName.hpp"
#include "core/PathUtil.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace vsm {
namespace {

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

std::string candidate_name(const BackupTimestamp &timestamp, const std::string &suffix,
                           unsigned int counter) {
  std::string candidate = make_timestamped_backup_name(timestamp, counter);
  if (!suffix.empty()) {
    candidate.insert(candidate.size() - 4, suffix);
  }
  return candidate;
}

bool contains_identity(const std::vector<std::string> &names, const std::string &identity) {
  return std::any_of(names.begin(), names.end(), [&](const std::string &name) {
    return backup_identity(name) == identity;
  });
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

std::string allocate_backup_name(const BackupTimestamp &timestamp, const std::string &suffix,
                                 const std::vector<std::string> &local_names,
                                 const std::vector<std::string> &remote_names) {
  for (unsigned int counter = 0;; counter = counter == 0 ? 2 : counter + 1) {
    const std::string candidate = candidate_name(timestamp, suffix, counter);
    const std::string identity = backup_identity(candidate);
    if (!contains_identity(local_names, identity) && !contains_identity(remote_names, identity)) {
      return candidate;
    }
  }
}

BackupCreationPlan plan_backup_creation(const BackupTimestamp &timestamp,
                                        const std::string &suffix,
                                        const std::vector<ArchiveEntryInfo> &current_entries,
                                        const std::string &backup_root,
                                        const std::string &save_id,
                                        const std::vector<std::string> &local_names,
                                        const std::vector<std::string> &remote_names,
                                        bool reuse_matching_archive) {
  for (unsigned int counter = 0;; counter = counter == 0 ? 2 : counter + 1) {
    const std::string candidate = candidate_name(timestamp, suffix, counter);
    const std::string identity = backup_identity(candidate);

    if (reuse_matching_archive) {
      for (const std::string &local_name : local_names) {
        if (backup_identity(local_name) != identity) {
          continue;
        }
        if (entries_match_backup_archive(
                current_entries,
                local_backup_archive_path(backup_root, save_id, local_name))) {
          return {local_name, true};
        }
      }
    }

    if (!contains_identity(local_names, identity) && !contains_identity(remote_names, identity)) {
      return {candidate, false};
    }
  }
}

bool publish_backup_download(const std::string &temporary_path,
                             const std::string &destination_path,
                             std::string *error) {
  if (temporary_path.empty() || destination_path.empty() ||
      temporary_path == destination_path) {
    if (error) {
      *error = "download paths are invalid";
    }
    return false;
  }

  // Reserve with a directory the backup scanner cannot mistake for a ZIP. If power is lost here,
  // the next attempt removes this empty marker and no zero-byte backup is ever shown.
  const std::string reservation_path = destination_path + ".publishing";
  if (rmdir(reservation_path.c_str()) != 0 && errno != ENOENT) {
    std::remove(temporary_path.c_str());
    if (error) {
      *error = "could not clear interrupted backup download";
    }
    return false;
  }
  if (mkdir(reservation_path.c_str(), 0777) != 0) {
    std::remove(temporary_path.c_str());
    if (error) {
      *error = "could not reserve backup path";
    }
    return false;
  }

  struct stat destination_info {};
  if (stat(destination_path.c_str(), &destination_info) == 0) {
    rmdir(reservation_path.c_str());
    std::remove(temporary_path.c_str());
    if (error) {
      *error = "backup already exists";
    }
    return false;
  }
  if (std::rename(temporary_path.c_str(), destination_path.c_str()) != 0) {
    rmdir(reservation_path.c_str());
    std::remove(temporary_path.c_str());
    if (error) {
      *error = "could not publish downloaded backup";
    }
    return false;
  }
  rmdir(reservation_path.c_str());
  if (error) {
    error->clear();
  }
  return true;
}

} // namespace vsm
