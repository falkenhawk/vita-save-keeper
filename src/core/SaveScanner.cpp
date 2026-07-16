#include "core/SaveScanner.hpp"

#include "core/PathUtil.hpp"
#include "core/SaveSlotMetadata.hpp"
#include "core/SfoParser.hpp"

#include <algorithm>
#include <cctype>
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

void sort_saves_by_display_name(std::vector<SaveRecord> *saves) {
  if (!saves) {
    return;
  }
  // Case-insensitive so "the Walking Dead" and "The Walking Dead" style titles interleave the way
  // a shelf would; ids break ties to keep the order stable between scans.
  const auto lower = [](const std::string &text) {
    std::string result = text;
    for (char &ch : result) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return result;
  };
  std::sort(saves->begin(), saves->end(), [&](const SaveRecord &a, const SaveRecord &b) {
    const std::string left = lower(a.display_name.empty() ? a.id : a.display_name);
    const std::string right = lower(b.display_name.empty() ? b.id : b.display_name);
    if (left != right) {
      return left < right;
    }
    return a.id < b.id;
  });
}

bool apply_mounted_save_time(SaveRecord *save, const SaveMetadata &metadata) {
  if (!save) {
    return false;
  }
  save->save_time_requires_mount = false;
  save->save_time_known = false;
  if (metadata.source != SaveTimeSource::VitaSlot ||
      !save_metadata_has_observed_time(metadata)) {
    return false;
  }
  save->saved_at = metadata.saved_at;
  save->saved_at_epoch = save_datetime_to_local_epoch(metadata.saved_at);
  save->save_time_known = true;
  return true;
}

void apply_save_sort(std::vector<SaveRecord> *saves, SaveSortMode mode,
                     const std::map<std::string, std::string> &newest_remote_by_folder) {
  if (!saves) {
    return;
  }
  if (mode == SaveSortMode::Name) {
    sort_saves_by_display_name(saves);
    return;
  }

  // Both time-based modes sort newest first and fall back to the name order among equals, so
  // ties (and saves with no timestamp at all) stay in a predictable order.
  sort_saves_by_display_name(saves);
  if (mode == SaveSortMode::LastSaved) {
    std::stable_sort(saves->begin(), saves->end(), [](const SaveRecord &a, const SaveRecord &b) {
      // Unknown and unresolved PFS times sort after every trustworthy time. Their raw mtimes may
      // belong to encryption bookkeeping and must not make them look recently saved.
      if (a.save_time_known != b.save_time_known) {
        return a.save_time_known;
      }
      return a.save_time_known && a.saved_at_epoch > b.saved_at_epoch;
    });
    return;
  }

  const auto newest_remote = [&](const SaveRecord &save) -> std::string {
    std::string key = normalize_path_component(save.id);
    if (key.empty()) {
      key = "unknown-save";
    }
    const auto found = newest_remote_by_folder.find(key);
    return found == newest_remote_by_folder.end() ? std::string() : found->second;
  };
  std::stable_sort(saves->begin(), saves->end(),
                   [&](const SaveRecord &a, const SaveRecord &b) {
                     return newest_remote(a) > newest_remote(b);
  });
}

bool save_sort_requires_all_times(SaveSortMode mode) {
  return mode == SaveSortMode::LastSaved;
}

const char *save_sort_mode_label(SaveSortMode mode) {
  switch (mode) {
  case SaveSortMode::LastSaved:
    return "Saved";
  case SaveSortMode::LastSynced:
    return "Backup";
  case SaveSortMode::Name:
  default:
    return "Name";
  }
}

std::string save_sort_mode_to_string(SaveSortMode mode) {
  switch (mode) {
  case SaveSortMode::LastSaved:
    return "saved";
  case SaveSortMode::LastSynced:
    return "synced";
  case SaveSortMode::Name:
  default:
    return "name";
  }
}

SaveSortMode save_sort_mode_from_string(const std::string &value) {
  if (value == "saved") {
    return SaveSortMode::LastSaved;
  }
  if (value == "synced") {
    return SaveSortMode::LastSynced;
  }
  return SaveSortMode::Name;
}

std::vector<SaveRecord> scan_save_roots(
    const std::vector<SaveRoot> &roots, const std::function<void()> &on_progress,
    const SaveMetadataResolver &resolve_metadata) {
  std::vector<SaveRecord> saves;
  const SaveMetadataResolver metadata_resolver =
      resolve_metadata ? resolve_metadata : resolve_save_metadata;

  for (const SaveRoot &root : roots) {
    const std::vector<std::string> child_directories = list_direct_child_directories(root.path);
    for (const std::string &child : child_directories) {
      SaveRecord save;
      save.platform = root.platform;
      save.id = child;
      save.display_name = child;
      save.path = join_path(root.path, child);
      save.save_time_requires_mount =
          save.platform != SavePlatform::Psp && save_directory_has_pfs_metadata(save.path);
      if (!save.save_time_requires_mount) {
        const SaveMetadata metadata =
            metadata_resolver(save.path, current_local_datetime());
        save.saved_at = metadata.saved_at;
        save.saved_at_epoch = save_datetime_to_local_epoch(metadata.saved_at);
        save.save_time_known = metadata.source != SaveTimeSource::BackupClock;
      }
      apply_sfo_metadata(&save);

      // Only direct children are treated as saves. Descending into save payloads would turn internal
      // folders such as sce_sys into fake titles and would make scanning much slower on a Vita.
      saves.push_back(std::move(save));
      if (on_progress) {
        on_progress();
      }
    }
  }

  return saves;
}

} // namespace vsm
