#include "core/TrackedFolders.hpp"

#include "core/PathUtil.hpp"
#include "core/SaveRecord.hpp"

#include <picojson.h>

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vsm {
namespace {

constexpr int kTrackedFoldersVersion = 1;

bool parse_path(const picojson::value &value, TrackedPath *path) {
  if (!value.is<picojson::object>()) {
    return false;
  }
  const picojson::object &object = value.get<picojson::object>();
  const auto path_field = object.find("path");
  if (path_field == object.end() || !path_field->second.is<std::string>() ||
      path_field->second.get<std::string>().empty()) {
    return false;
  }
  path->path = path_field->second.get<std::string>();
  const auto prefix_field = object.find("prefix");
  path->prefix = (prefix_field != object.end() && prefix_field->second.is<std::string>())
                     ? prefix_field->second.get<std::string>()
                     : "";
  return true;
}

// false for a non-object value, a missing/empty/unsafe id, a missing/non-array paths field, or
// when every path in the entry failed to parse - callers skip the entry rather than fail the
// whole document.
bool parse_entry(const picojson::value &value, TrackedFolderEntry *entry) {
  if (!value.is<picojson::object>()) {
    return false;
  }
  const picojson::object &object = value.get<picojson::object>();
  const auto id_field = object.find("id");
  if (id_field == object.end() || !id_field->second.is<std::string>() ||
      id_field->second.get<std::string>().empty()) {
    return false;
  }
  const std::string &id = id_field->second.get<std::string>();
  // ids double as a backup directory name and a Drive folder key, so a hand-edited file can't be
  // allowed to smuggle a separator or a "." / ".." traversal into that single path segment.
  if (id == "." || id == ".." || id.find_first_of("/\\:") != std::string::npos) {
    return false;
  }
  entry->id = id;
  const auto title_field = object.find("title");
  entry->title = (title_field != object.end() && title_field->second.is<std::string>())
                     ? title_field->second.get<std::string>()
                     : "";
  const auto paths_field = object.find("paths");
  if (paths_field == object.end() || !paths_field->second.is<picojson::array>()) {
    return false;
  }
  for (const picojson::value &path_value : paths_field->second.get<picojson::array>()) {
    TrackedPath path;
    if (parse_path(path_value, &path)) {
      entry->paths.push_back(std::move(path));
    }
  }
  return !entry->paths.empty();
}

} // namespace

TrackedFoldersParseResult parse_tracked_folders_json(const std::string &text) {
  TrackedFoldersParseResult result;
  picojson::value root;
  const std::string parse_error = picojson::parse(root, text);
  if (!parse_error.empty() || !root.is<picojson::object>()) {
    return result;
  }
  const picojson::object &root_object = root.get<picojson::object>();
  const auto entries_field = root_object.find("entries");
  if (entries_field == root_object.end() || !entries_field->second.is<picojson::array>()) {
    return result;
  }
  // first entry with a given id wins; a later duplicate would collide as a backup/Drive key.
  std::set<std::string> seen_ids;
  for (const picojson::value &entry_value : entries_field->second.get<picojson::array>()) {
    TrackedFolderEntry entry;
    if (parse_entry(entry_value, &entry) && seen_ids.insert(entry.id).second) {
      result.config.entries.push_back(std::move(entry));
    }
  }
  const auto hidden_field = root_object.find("hidden");
  if (hidden_field != root_object.end() && hidden_field->second.is<picojson::array>()) {
    for (const picojson::value &id_value : hidden_field->second.get<picojson::array>()) {
      if (id_value.is<std::string>() && !id_value.get<std::string>().empty()) {
        result.config.hidden_ids.insert(id_value.get<std::string>());
      }
    }
  }
  result.ok = true;
  return result;
}

std::string serialize_tracked_folders_json(const TrackedFoldersConfig &config) {
  picojson::object root;
  root["version"] = picojson::value(static_cast<double>(kTrackedFoldersVersion));
  picojson::array entries;
  for (const TrackedFolderEntry &entry : config.entries) {
    picojson::object object;
    object["id"] = picojson::value(entry.id);
    object["title"] = picojson::value(entry.title);
    picojson::array paths;
    for (const TrackedPath &path : entry.paths) {
      picojson::object path_object;
      path_object["prefix"] = picojson::value(path.prefix);
      path_object["path"] = picojson::value(path.path);
      paths.push_back(picojson::value(std::move(path_object)));
    }
    object["paths"] = picojson::value(std::move(paths));
    entries.push_back(picojson::value(std::move(object)));
  }
  root["entries"] = picojson::value(std::move(entries));
  // a std::set already iterates in sorted order, so this array stays stable across rewrites
  picojson::array hidden;
  for (const std::string &id : config.hidden_ids) {
    hidden.push_back(picojson::value(id));
  }
  root["hidden"] = picojson::value(std::move(hidden));
  return picojson::value(std::move(root)).serialize(true);
}

std::string make_tracked_entry_id(const std::string &folder_name,
                                  const std::set<std::string> &taken_ids) {
  const std::string normalized = normalize_path_component(folder_name);
  const std::string base = normalized.empty() ? "data-folder" : "data-" + normalized;
  if (taken_ids.count(base) == 0) {
    return base;
  }
  for (int suffix = 2;; ++suffix) {
    const std::string candidate = base + "-" + std::to_string(suffix);
    if (taken_ids.count(candidate) == 0) {
      return candidate;
    }
  }
}

SaveMetadata resolve_tracked_metadata(const std::vector<std::string> &paths,
                                      const SaveDateTime &backup_clock) {
  if (paths.empty()) {
    return resolve_save_metadata("", backup_clock);
  }
  SaveMetadata newest = resolve_save_metadata(paths.front(), backup_clock);
  long long newest_epoch = save_datetime_to_local_epoch(newest.saved_at);
  for (std::size_t i = 1; i < paths.size(); ++i) {
    const SaveMetadata candidate = resolve_save_metadata(paths[i], backup_clock);
    if (!save_metadata_has_observed_time(candidate)) {
      continue;
    }
    const long long candidate_epoch = save_datetime_to_local_epoch(candidate.saved_at);
    if (!save_metadata_has_observed_time(newest) || candidate_epoch > newest_epoch) {
      newest = candidate;
      newest_epoch = candidate_epoch;
    }
  }
  return newest;
}

std::vector<SaveRecord>
build_tracked_save_records(const TrackedFoldersConfig &config,
                           const std::function<bool(const std::string &)> &directory_exists) {
  std::vector<SaveRecord> records;
  constexpr const char *kRetroArchId = "data-retroarch";
  constexpr const char *kRetroArchRoot = "ux0:data/retroarch";

  if (directory_exists(kRetroArchRoot)) {
    SaveRecord record;
    record.id = kRetroArchId;
    record.display_name = "RetroArch";
    record.path = kRetroArchRoot;
    record.tracked_paths = {
        {"savefiles", std::string(kRetroArchRoot) + "/savefiles"},
        {"savestates", std::string(kRetroArchRoot) + "/savestates"},
    };
    records.push_back(std::move(record));
  }

  for (const TrackedFolderEntry &entry : config.entries) {
    // The builtin above already covers this id; a stale hand-edited config entry must never
    // duplicate it, whether or not the builtin's own directory happened to exist.
    if (entry.id == kRetroArchId) {
      continue;
    }
    SaveRecord record;
    record.id = entry.id;
    record.display_name = entry.title;
    record.tracked_paths = entry.paths;
    if (!entry.paths.empty()) {
      record.path = entry.paths.front().path;
    }
    records.push_back(std::move(record));
  }

  return records;
}

} // namespace vsm
