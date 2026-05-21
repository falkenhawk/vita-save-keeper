#include "core/SaveScanner.hpp"

#include "core/SfoParser.hpp"

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

bool is_regular_file(const std::string &path) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    return false;
  }
  return S_ISREG(info.st_mode);
}

std::string first_existing_file(const std::vector<std::string> &paths) {
  for (const std::string &path : paths) {
    if (is_regular_file(path)) {
      return path;
    }
  }
  return {};
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

void apply_sfo_metadata(SaveRecord *save) {
  const std::string vita_sfo = first_existing_file({
      join_path(join_path(save->path, "sce_sys"), "param.sfo"),
      join_path(join_path(save->path, "sce_sys"), "PARAM.SFO"),
  });
  const std::string psp_sfo = first_existing_file({
      join_path(save->path, "PARAM.SFO"),
      join_path(save->path, "param.sfo"),
  });
  const std::string sfo_path = save->platform == SavePlatform::Psp ? psp_sfo : vita_sfo;
  if (!sfo_path.empty()) {
    const SfoMetadata metadata = parse_sfo_metadata_file(sfo_path);
    const auto title_id = metadata.strings.find("TITLE_ID");
    if (title_id != metadata.strings.end() && !title_id->second.empty()) {
      save->title_id = title_id->second;
    }

    const std::string title = title_from_sfo_metadata(metadata);
    if (!title.empty()) {
      save->display_name = title;
    }
  }

  if (save->platform == SavePlatform::Psp) {
    save->icon_path = first_existing_file({
        join_path(save->path, "ICON0.PNG"),
        join_path(save->path, "icon0.png"),
    });
  } else {
    const std::string metadata_title_id =
        save->title_id.empty() ? save->id : save->title_id;
    if (save->display_name == save->id) {
      const std::string appmeta_sfo = first_existing_file({
          join_path(join_path("ur0:appmeta", metadata_title_id), "param.sfo"),
          join_path(join_path("ur0:appmeta", metadata_title_id), "PARAM.SFO"),
          join_path(join_path("ux0:appmeta", metadata_title_id), "param.sfo"),
          join_path(join_path("ux0:appmeta", metadata_title_id), "PARAM.SFO"),
      });
      if (!appmeta_sfo.empty()) {
        const SfoMetadata metadata = parse_sfo_metadata_file(appmeta_sfo);
        const std::string title = title_from_sfo_metadata(metadata);
        if (!title.empty()) {
          save->display_name = title;
        }
      }
    }
    save->icon_path = first_existing_file({
        join_path(join_path(save->path, "sce_sys"), "icon0.png"),
        join_path(join_path(save->path, "sce_sys"), "ICON0.PNG"),
        join_path(join_path("ur0:appmeta", metadata_title_id), "icon0.png"),
        join_path(join_path("ux0:appmeta", metadata_title_id), "icon0.png"),
    });
  }
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
      apply_sfo_metadata(&save);

      // Only direct children are treated as saves. Descending into save payloads would turn internal
      // folders such as sce_sys into fake titles and would make scanning much slower on a Vita.
      saves.push_back(std::move(save));
    }
  }

  return saves;
}

} // namespace vsm
