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

std::string serialize_save_time_cache(const SaveTimeCache &cache) {
  picojson::object root;
  root["version"] = picojson::value(static_cast<double>(kSaveTimeCacheVersion));
  picojson::object entries;
  for (const auto &item : cache.entries) {
    const SaveTimeCacheEntry &entry = item.second;
    if (!entry.fingerprint.ok) {
      continue;
    }
    picojson::object object;
    object["newestMtime"] =
        picojson::value(static_cast<double>(entry.fingerprint.newest_mtime));
    object["fileCount"] = picojson::value(static_cast<double>(entry.fingerprint.file_count));
    object["totalBytes"] = picojson::value(static_cast<double>(entry.fingerprint.total_bytes));
    // A "resolved to no readable time" entry simply carries no savedAt.
    if (entry.has_time) {
      object["savedAt"] = picojson::value(format_save_datetime(entry.saved_at));
    }
    entries[item.first] = picojson::value(std::move(object));
  }
  root["entries"] = picojson::value(std::move(entries));
  return picojson::value(std::move(root)).serialize(true);
}

bool parse_save_time_cache(const std::string &json, SaveTimeCache *cache) {
  cache->entries.clear();
  picojson::value root;
  const std::string parse_error = picojson::parse(root, json);
  if (!parse_error.empty() || !root.is<picojson::object>()) {
    return false;
  }
  const picojson::object &root_object = root.get<picojson::object>();
  const auto version = root_object.find("version");
  if (version == root_object.end() || !version->second.is<double>() ||
      static_cast<int>(version->second.get<double>()) != kSaveTimeCacheVersion) {
    return false;
  }
  const auto entries = root_object.find("entries");
  if (entries == root_object.end() || !entries->second.is<picojson::object>()) {
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
    SaveTimeCacheEntry entry;
    entry.fingerprint.ok = true;
    if (!number("newestMtime", &entry.fingerprint.newest_mtime) ||
        !number("fileCount", &entry.fingerprint.file_count) ||
        !number("totalBytes", &entry.fingerprint.total_bytes)) {
      continue;
    }
    const auto saved_at = object.find("savedAt");
    if (saved_at != object.end()) {
      if (!saved_at->second.is<std::string>() ||
          !parse_save_datetime(saved_at->second.get<std::string>(), &entry.saved_at)) {
        continue;
      }
      entry.has_time = true;
    }
    cache->entries[item.first] = std::move(entry);
  }
  return true;
}

SaveTimeCache read_save_time_cache(const std::string &path) {
  SaveTimeCache cache;
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return cache;
  }
  std::string json;
  char buffer[4096];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    json.append(buffer, read);
    if (json.size() > kMaxSaveTimeCacheSize) {
      std::fclose(file);
      return cache;
    }
  }
  std::fclose(file);
  parse_save_time_cache(json, &cache);
  return cache;
}

bool write_save_time_cache_atomic(const std::string &path, const SaveTimeCache &cache,
                                  std::string *error) {
  const std::string json = serialize_save_time_cache(cache);
  if (json.size() > kMaxSaveTimeCacheSize) {
    if (error) *error = "cache too large";
    return false;
  }
  const std::string temporary = path + ".tmp";
  FILE *file = std::fopen(temporary.c_str(), "wb");
  if (!file) {
    if (error) *error = "could not create cache file";
    return false;
  }
  // Temp file then rename, same as the metadata sidecars: a reader never sees a half-written
  // cache, and durability across a power cut is not worth an fsync - the cache rebuilds itself.
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

} // namespace vsm
