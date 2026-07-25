#include "core/SaveTimeCache.hpp"

#include "core/PathUtil.hpp"

#include <picojson.h>

#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <utility>

namespace vsm {
namespace {

bool is_dot_entry(const char *name) {
  return std::string(name) == "." || std::string(name) == "..";
}

// Any unreadable file or directory poisons the fingerprint (ok = false): a partial reading could
// match a complete one taken earlier and wrongly validate a cache entry. Not-ok fingerprints
// never match, which degrades to reading the time through a mount - correct, just slower.
bool add_fingerprint(const std::string &path, SaveFingerprint *fingerprint) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    return false;
  }
  if (S_ISREG(info.st_mode)) {
    ++fingerprint->file_count;
    fingerprint->total_bytes += static_cast<long long>(info.st_size);
    if (static_cast<long long>(info.st_mtime) > fingerprint->newest_mtime) {
      fingerprint->newest_mtime = static_cast<long long>(info.st_mtime);
    }
    return true;
  }
  if (!S_ISDIR(info.st_mode)) {
    return true;
  }
  DIR *directory = opendir(path.c_str());
  if (!directory) {
    return false;
  }
  bool ok = true;
  while (dirent *entry = readdir(directory)) {
    if (!is_dot_entry(entry->d_name) &&
        !add_fingerprint(join_path(path, entry->d_name), fingerprint)) {
      ok = false;
    }
  }
  closedir(directory);
  return ok;
}

} // namespace

SaveFingerprint compute_save_fingerprint(const std::string &save_path) {
  SaveFingerprint fingerprint;
  fingerprint.ok = add_fingerprint(save_path, &fingerprint) && fingerprint.file_count > 0;
  return fingerprint;
}

namespace {

// Temp file then rename, same as the metadata sidecars: a reader never sees a half-written
// cache, and durability across a power cut is not worth an fsync - the caches rebuild themselves.
bool write_json_atomic(const std::string &path, const std::string &json, std::string *error) {
  if (json.size() > kMaxSaveIndexSize) {
    if (error) *error = "cache too large";
    return false;
  }
  const std::string temporary = path + ".tmp";
  FILE *file = std::fopen(temporary.c_str(), "wb");
  if (!file) {
    if (error) *error = "could not create cache file";
    return false;
  }
  bool wrote = std::fwrite(json.data(), 1, json.size(), file) == json.size();
  wrote = std::fflush(file) == 0 && wrote;
  wrote = std::fclose(file) == 0 && wrote;
  if (!wrote) {
    std::remove(temporary.c_str());
    if (error) *error = "could not write cache file";
    return false;
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    if (error) *error = "could not replace cache file";
    return false;
  }
  if (error) error->clear();
  return true;
}

std::string read_bounded_file(const std::string &path) {
  std::string content;
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return content;
  }
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    content.append(buffer, read);
    if (content.size() > kMaxSaveIndexSize) {
      content.clear();
      break;
    }
  }
  std::fclose(file);
  return content;
}

} // namespace

std::string serialize_save_index(const SaveIndex &index) {
  picojson::object root;
  root["version"] = picojson::value(static_cast<double>(kSaveIndexVersion));
  root["appDbMtime"] = picojson::value(static_cast<double>(index.app_db_mtime));
  root["appDbSize"] = picojson::value(static_cast<double>(index.app_db_size));
  picojson::object entries;
  for (const auto &item : index.entries) {
    const SaveIndexEntry &entry = item.second;
    picojson::object object;
    object["newestMtime"] =
        picojson::value(static_cast<double>(entry.fingerprint.newest_mtime));
    object["fileCount"] = picojson::value(static_cast<double>(entry.fingerprint.file_count));
    object["totalBytes"] = picojson::value(static_cast<double>(entry.fingerprint.total_bytes));
    if (!entry.fingerprint.ok) {
      // A partial walk's numbers can coincide with a complete one later; the flag must survive
      // the round-trip so matches() keeps refusing it, while the entry itself survives (an
      // app-db title outlives an unreadable folder).
      object["fingerprintOk"] = picojson::value(false);
    }
    if (entry.time_resolved) {
      // null marks "resolved, nothing readable"; an unresolved time has no savedAt at all.
      object["savedAt"] = entry.has_time
                              ? picojson::value(format_save_datetime(entry.saved_at))
                              : picojson::value();
    }
    object["displayName"] = picojson::value(entry.display_name);
    if (entry.title_id != item.first) {
      object["titleId"] = picojson::value(entry.title_id);
    }
    object["iconPath"] = picojson::value(entry.icon_path);
    object["fromAppDb"] = picojson::value(entry.from_app_db);
    entries[item.first] = picojson::value(std::move(object));
  }
  root["entries"] = picojson::value(std::move(entries));
  return picojson::value(std::move(root)).serialize(true);
}

bool parse_save_index(const std::string &json, SaveIndex *index) {
  *index = {};
  picojson::value root;
  const std::string parse_error = picojson::parse(root, json);
  if (!parse_error.empty() || !root.is<picojson::object>()) {
    return false;
  }
  const picojson::object &root_object = root.get<picojson::object>();
  const auto root_number = [&root_object](const char *key, long long *value) {
    const auto found = root_object.find(key);
    if (found == root_object.end() || !found->second.is<double>()) {
      return false;
    }
    *value = static_cast<long long>(found->second.get<double>());
    return true;
  };
  // Version 1 is the old titles-only save-titles.json, read forward. Its entries carry the same
  // fields minus savedAt, so one uniform entry parser below covers both versions.
  long long version = 0;
  if (!root_number("version", &version) || (version != 1 && version != kSaveIndexVersion) ||
      !root_number("appDbMtime", &index->app_db_mtime) ||
      !root_number("appDbSize", &index->app_db_size)) {
    *index = {};
    return false;
  }
  const auto entries = root_object.find("entries");
  if (entries == root_object.end() || !entries->second.is<picojson::object>()) {
    *index = {};
    return false;
  }
  for (const auto &item : entries->second.get<picojson::object>()) {
    if (!item.second.is<picojson::object>()) {
      continue;
    }
    const picojson::object &object = item.second.get<picojson::object>();
    const auto number = [&object](const char *key, long long *value) {
      const auto found = object.find(key);
      if (found == object.end() || !found->second.is<double>()) {
        return false;
      }
      *value = static_cast<long long>(found->second.get<double>());
      return *value >= 0;
    };
    const auto text = [&object](const char *key, std::string *value) {
      const auto found = object.find(key);
      if (found == object.end() || !found->second.is<std::string>()) {
        return false;
      }
      *value = found->second.get<std::string>();
      return true;
    };
    SaveIndexEntry entry;
    entry.fingerprint.ok = true;
    const auto from_db = object.find("fromAppDb");
    if (from_db == object.end() || !from_db->second.is<bool>() ||
        !number("newestMtime", &entry.fingerprint.newest_mtime) ||
        !number("fileCount", &entry.fingerprint.file_count) ||
        !number("totalBytes", &entry.fingerprint.total_bytes) ||
        !text("displayName", &entry.display_name) || !text("iconPath", &entry.icon_path)) {
      continue;
    }
    entry.from_app_db = from_db->second.get<bool>();
    // Absent means a complete walk; version-1 files never carry it. A wrong type is a malformed
    // entry, not "assume true".
    const auto fingerprint_ok = object.find("fingerprintOk");
    if (fingerprint_ok != object.end()) {
      if (!fingerprint_ok->second.is<bool>()) {
        continue;
      }
      entry.fingerprint.ok = fingerprint_ok->second.get<bool>();
    }
    // An omitted titleId means "equals the save id"; an explicit value (even "") wins.
    const auto title_id = object.find("titleId");
    if (title_id != object.end()) {
      if (!title_id->second.is<std::string>()) {
        continue;
      }
      entry.title_id = title_id->second.get<std::string>();
    } else {
      entry.title_id = item.first;
    }
    const auto saved_at = object.find("savedAt");
    if (saved_at != object.end()) {
      if (saved_at->second.is<std::string>()) {
        if (!parse_save_datetime(saved_at->second.get<std::string>(), &entry.saved_at)) {
          continue;
        }
        entry.has_time = true;
      } else if (!saved_at->second.is<picojson::null>()) {
        continue;
      }
      entry.time_resolved = true;
    }
    index->entries[item.first] = std::move(entry);
  }
  return true;
}

SaveIndex read_save_index(const std::string &path) {
  SaveIndex index;
  parse_save_index(read_bounded_file(path), &index);
  return index;
}

bool write_save_index_atomic(const std::string &path, const SaveIndex &index,
                             std::string *error) {
  return write_json_atomic(path, serialize_save_index(index), error);
}

bool stat_file_stamp(const std::string &path, long long *mtime, long long *size) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return false;
  }
  if (mtime) *mtime = static_cast<long long>(info.st_mtime);
  if (size) *size = static_cast<long long>(info.st_size);
  return true;
}

} // namespace vsm
