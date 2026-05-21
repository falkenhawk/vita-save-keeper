#include "core/SaveScanner.hpp"

#include <algorithm>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace vsm {
namespace {

bool is_dot_entry(const char *name) {
  return std::string(name) == "." || std::string(name) == "..";
}

std::string join_path(const std::string &parent, const std::string &child) {
  if (parent.empty()) {
    return child;
  }
  if (parent.back() == '/') {
    return parent + child;
  }
  return parent + "/" + child;
}

bool is_directory(const std::string &path) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    return false;
  }
  return S_ISDIR(info.st_mode);
}

std::vector<std::string> list_direct_child_directories(const std::string &root_path) {
  std::vector<std::string> directories;
  DIR *dir = opendir(root_path.c_str());
  if (!dir) {
    // Save roots vary by model, storage setup, and installed plugins. Treating missing roots as an
    // empty list keeps the UI usable on systems without Adrenaline or without a mounted partition.
    return directories;
  }

  while (dirent *entry = readdir(dir)) {
    if (is_dot_entry(entry->d_name)) {
      continue;
    }

    const std::string child_name = entry->d_name;
    const std::string child_path = join_path(root_path, child_name);
    if (is_directory(child_path)) {
      directories.push_back(child_name);
    }
  }
  closedir(dir);

  std::sort(directories.begin(), directories.end());
  return directories;
}

} // namespace

std::vector<SaveRecord> scan_save_roots(const std::vector<SaveRoot> &roots) {
  std::vector<SaveRecord> saves;

  for (const SaveRoot &root : roots) {
    const std::vector<std::string> child_directories = list_direct_child_directories(root.path);
    for (const std::string &child : child_directories) {
      SaveRecord save;
      save.platform = root.platform;
      save.id = child;
      save.display_name = child;
      save.path = join_path(root.path, child);

      // Only direct children are treated as saves. Descending into save payloads would turn internal
      // folders such as sce_sys into fake titles and would make scanning much slower on a Vita.
      saves.push_back(std::move(save));
    }
  }

  return saves;
}

} // namespace vsm
