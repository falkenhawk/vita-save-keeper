#include "core/TrackedFolders.hpp"

#include "core/BackupArchive.hpp"
#include "core/PathUtil.hpp"
#include "core/SaveRecord.hpp"

#include <picojson.h>

#include <algorithm>

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace vsm {

const char *const kSavedataPrefix = "savedata";

namespace {

constexpr int kTrackedFoldersVersion = 1;

// A containment match needs this many folded characters on both sides. Without a floor, a folder
// like "jav" would latch onto any title that happens to contain those letters.
constexpr std::size_t kMinContainmentLength = 4;

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

// false for a non-object value, a missing/empty/unsafe id, or a missing/non-array paths field -
// callers skip the entry rather than fail the whole document. Individual malformed paths are
// dropped; the entry survives with the rest.
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
  // An entry that keeps no paths is still valid: in the user file it is the tombstone that
  // suppresses the base entry with the same id.
  return true;
}

void read_id_array(const picojson::object &root, const char *field, std::set<std::string> *ids) {
  const auto found = root.find(field);
  if (found == root.end() || !found->second.is<picojson::array>()) {
    return;
  }
  for (const picojson::value &id_value : found->second.get<picojson::array>()) {
    if (id_value.is<std::string>() && !id_value.get<std::string>().empty()) {
      ids->insert(id_value.get<std::string>());
    }
  }
}

// A restore destination is safe only when it is confined to ux0:data (where extra folders live)
// with every "/"-separated segment non-empty and not "..". Rejecting empty segments closes the
// "ux0:data/" and "ux0:data//x" spellings that would resolve back to the data root and let a restore
// clear it wholesale; rejecting ".." blocks climbing out of the sandbox.
//
// Checked first, ahead of that prefix/segment walk: any control character (byte < 0x20), which
// includes an embedded NUL. picojson happily decodes a JSON \u0000 escape into a real NUL byte
// inside a std::string, so a sidecar can carry a path like "ux0:data/\0.. junk" that this function's
// std::string-based checks see as a harmless "ux0:data/.. junk"-adjacent string, while the
// filesystem layer (clear_directory_contents, which goes through .c_str()) sees only
// "ux0:data/" or "ux0:data/.." up to the NUL. That mismatch is exactly the gap a crafted sidecar
// exploits to make an accepted path collapse to the data root (or climb out of it) at the syscall
// boundary, so a restore driven by that sidecar can wipe the whole ux0:data tree. No legitimate
// Vita path contains a control character, so rejecting all of them (not just NUL) is the safer floor.
bool destination_is_confined(const std::string &path) {
  for (const char ch : path) {
    if (static_cast<unsigned char>(ch) < 0x20) {
      return false;
    }
  }
  if (path.compare(0, 9, "ux0:data/") != 0) {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end = slash == std::string::npos ? path.size() : slash;
    const std::string segment = path.substr(start, end - start);
    if (segment.empty() || segment == "..") {
      return false;
    }
    if (slash == std::string::npos) {
      return true;
    }
    start = slash + 1;
  }
  return true;
}

// Lowercased alphanumerics only, so "AutoPlugin II", "auto-plugin_ii" and "AUTOPLUGINII" all fold
// together. Separators carry no meaning in the ux0:data naming convention.
std::string fold_name(const std::string &input) {
  std::string folded;
  for (const char ch : input) {
    const unsigned char value = static_cast<unsigned char>(ch);
    if (std::isalnum(value)) {
      folded.push_back(static_cast<char>(std::tolower(value)));
    }
  }
  return folded;
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
  // "hidden" is the earlier draft's spelling of the same list. Reading both means a config written
  // by that build keeps its exclusions; only "skipped" is ever written back.
  read_id_array(root_object, "skipped", &result.config.skipped_ids);
  read_id_array(root_object, "hidden", &result.config.skipped_ids);
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
  picojson::array skipped;
  for (const std::string &id : config.skipped_ids) {
    skipped.push_back(picojson::value(id));
  }
  root["skipped"] = picojson::value(std::move(skipped));
  return picojson::value(std::move(root)).serialize(true);
}

namespace {

// Mirrors write_json_atomic in SaveTimeCache.cpp: that helper has internal linkage there, so it is
// not reachable from this translation unit and this is a minimal copy of the same temp-file-then-
// rename mechanics rather than a shared call. No size cap here - unlike the save-time cache, this
// config's size is bounded by how many folders the user has added, not by scan results.
bool write_json_atomic(const std::string &path, const std::string &json, std::string *error) {
  const std::string temporary = path + ".tmp";
  FILE *file = std::fopen(temporary.c_str(), "wb");
  if (!file) {
    if (error) *error = "could not create config file";
    return false;
  }
  bool wrote = std::fwrite(json.data(), 1, json.size(), file) == json.size();
  wrote = std::fflush(file) == 0 && wrote;
  wrote = std::fclose(file) == 0 && wrote;
  if (!wrote) {
    std::remove(temporary.c_str());
    if (error) *error = "could not write config file";
    return false;
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    if (error) *error = "could not replace config file";
    return false;
  }
  if (error) error->clear();
  return true;
}

} // namespace

bool write_tracked_folders_json_atomic(const std::string &path, const TrackedFoldersConfig &config,
                                       std::string *error) {
  return write_json_atomic(path, serialize_tracked_folders_json(config), error);
}

std::string make_extra_prefix(const std::string &folder_name,
                              const std::set<std::string> &taken_prefixes) {
  const std::string normalized = normalize_path_component(folder_name);
  const std::string base = normalized.empty() ? "folder" : normalized;
  const auto is_taken = [&](const std::string &candidate) {
    return candidate == kSavedataPrefix || taken_prefixes.count(candidate) != 0;
  };
  if (!is_taken(base)) {
    return base;
  }
  for (int suffix = 2;; ++suffix) {
    const std::string candidate = base + "-" + std::to_string(suffix);
    if (!is_taken(candidate)) {
      return candidate;
    }
  }
}

ArchiveLayout archive_layout_for_record(const SaveRecord &record) {
  ArchiveLayout layout;
  const bool has_savedata = !record.path.empty();
  const std::size_t count = (has_savedata ? 1u : 0u) + record.extra_paths.size();
  if (count == 0) {
    return layout;
  }
  if (count == 1) {
    // A sole directory keeps the flat layout an ordinary savedata backup uses, whichever kind of
    // directory it is. For a savedata-only entry that is what keeps new archives byte-identical to
    // the ones 1.1.1 writes; for an extras-only app it is simply the same rule applied.
    if (has_savedata) {
      layout.sources.push_back({std::string(), record.path});
    } else {
      layout.sources.push_back({std::string(), record.extra_paths.front().path});
      layout.extra_targets = layout.sources;
    }
    return layout;
  }
  if (has_savedata) {
    layout.sources.push_back({kSavedataPrefix, record.path});
  }
  for (const TrackedPath &extra : record.extra_paths) {
    layout.sources.push_back(extra);
    layout.extra_targets.push_back(extra);
  }
  return layout;
}

std::vector<TrackedPath> restore_targets_for_backup(const std::string &savedata_path,
                                                    const std::vector<TrackedPath> &extra_targets) {
  std::vector<TrackedPath> targets;
  if (extra_targets.empty()) {
    return targets;
  }
  // A single extra carrying an empty prefix means the archive was written flat, by an app that had
  // no savedata folder at the time. Prepending a savedata target would both contradict that layout
  // and break the "empty prefix only when sole" rule, so the entry's savedata (acquired since) is
  // deliberately left out of this backup's restore.
  const bool archive_is_flat = extra_targets.size() == 1 && extra_targets.front().prefix.empty();
  if (!savedata_path.empty() && !archive_is_flat) {
    targets.push_back({kSavedataPrefix, savedata_path});
  }
  for (const TrackedPath &extra : extra_targets) {
    targets.push_back(extra);
  }
  return targets;
}

bool tracked_targets_are_safe(const std::vector<TrackedPath> &targets) {
  for (const TrackedPath &target : targets) {
    if (!destination_is_confined(target.path)) {
      return false;
    }
  }
  // Reuse the same prefix rule the archive writer and reader enforce (BackupArchive.hpp), so a
  // sidecar can only map prefixes back the way create_backup_archive could have written them.
  return tracked_paths_are_well_formed(targets);
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

bool tracked_paths_equal(const std::vector<TrackedPath> &a, const std::vector<TrackedPath> &b) {
  if (a.size() != b.size()) {
    return false;
  }
  // Sets are tiny (a handful of folders per app), so a quadratic scan beats sorting copies.
  std::vector<bool> used(b.size(), false);
  for (const TrackedPath &left : a) {
    bool found = false;
    for (std::size_t i = 0; i < b.size(); ++i) {
      if (!used[i] && b[i].prefix == left.prefix && b[i].path == left.path) {
        used[i] = true;
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

std::vector<TrackedFolderEntry>
effective_data_folder_entries(const TrackedFoldersConfig &base, const TrackedFoldersConfig &user,
                              const std::function<bool(const std::string &)> &directory_exists) {
  std::vector<TrackedFolderEntry> effective;
  std::set<std::string> user_ids;
  for (const TrackedFolderEntry &entry : user.entries) {
    user_ids.insert(entry.id);
  }
  for (const TrackedFolderEntry &entry : base.entries) {
    if (user_ids.count(entry.id) != 0) {
      continue;  // overridden (or tombstoned) below
    }
    bool any_exists = false;
    for (const TrackedPath &path : entry.paths) {
      if (directory_exists && directory_exists(path.path)) {
        any_exists = true;
        break;
      }
    }
    if (any_exists) {
      effective.push_back(entry);
    }
  }
  for (const TrackedFolderEntry &entry : user.entries) {
    if (!entry.paths.empty()) {
      effective.push_back(entry);
    }
  }
  return effective;
}

void update_data_folder_override(TrackedFoldersConfig *user, const std::string &id,
                                 const std::string &title,
                                 const std::vector<TrackedPath> &new_paths,
                                 const std::vector<TrackedPath> *base_paths) {
  auto &entries = user->entries;
  const auto found =
      std::find_if(entries.begin(), entries.end(),
                   [&](const TrackedFolderEntry &entry) { return entry.id == id; });
  const bool matches_base = base_paths && tracked_paths_equal(new_paths, *base_paths);
  const bool remove_entry = matches_base || (new_paths.empty() && !base_paths);
  if (remove_entry) {
    // Fall back to the base (or to nothing): with no override stored, a future version's updated
    // base entry applies as shipped.
    if (found != entries.end()) {
      entries.erase(found);
    }
    return;
  }
  // Store the override - possibly the empty tombstone that suppresses an applicable base entry.
  if (found != entries.end()) {
    found->title = title;
    found->paths = new_paths;
  } else {
    entries.push_back({id, title, new_paths});
  }
}

std::vector<SaveRecord>
build_orphan_app_records(const TrackedFoldersConfig &config,
                         const std::function<bool(const std::string &)> &is_scanned_id) {
  std::vector<SaveRecord> records;
  for (const TrackedFolderEntry &entry : config.entries) {
    // The scan already produced a record for this app, so its folders belong on that record; only
    // an app with no savedata folder at all needs one synthesized here.
    if (is_scanned_id && is_scanned_id(entry.id)) {
      continue;
    }
    SaveRecord record;
    record.id = entry.id;
    record.display_name = entry.title.empty() ? entry.id : entry.title;
    // No savedata folder: path stays empty, which archive_layout_for_record reads as extras-only.
    record.extra_paths = entry.paths;
    records.push_back(std::move(record));
  }
  return records;
}

std::string best_data_folder_match(const std::string &app_title,
                                   const std::vector<std::string> &folder_names) {
  const std::string title = fold_name(app_title);
  if (title.empty()) {
    return {};
  }
  std::string best;
  std::size_t best_length = 0;
  for (const std::string &name : folder_names) {
    const std::string folded = fold_name(name);
    if (folded.empty()) {
      continue;
    }
    if (folded == title) {
      // An exact fold is as good as it gets; no longer candidate can beat it.
      return name;
    }
    if (folded.size() < kMinContainmentLength || title.size() < kMinContainmentLength) {
      continue;
    }
    if (title.find(folded) == std::string::npos && folded.find(title) == std::string::npos) {
      continue;
    }
    // Among containment matches the longest folder name is the most specific: "VitaGrafix" beats a
    // hypothetical "Vita" for "VitaGrafix Configurator".
    if (folded.size() > best_length) {
      best = name;
      best_length = folded.size();
    }
  }
  return best;
}

} // namespace vsm
