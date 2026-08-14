#include "core/DirWalk.hpp"

#include "core/DiagTrace.hpp"
#include "core/PathUtil.hpp"

#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>

#ifdef __vita__
#include <psp2/io/stat.h>
#endif

namespace vsm {
namespace {

bool is_dot_entry(const char *name) {
  return std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0;
}

#ifdef __vita__
long long sce_datetime_to_epoch(const SceDateTime &value) {
  // SceIoStat carries local calendar fields; mktime is the same local-fields-to-epoch
  // conversion save_datetime_to_local_epoch uses. Whether this matches newlib's stat() exactly
  // is verified on hardware by diffing fingerprint newest-mtime values in the diagnostic trace
  // between a stat()-based build and this one - a mismatch would only cost a one-time index
  // re-resolve, but it must be known, not assumed.
  std::tm local {};
  local.tm_year = value.year - 1900;
  local.tm_mon = value.month - 1;
  local.tm_mday = value.day;
  local.tm_hour = value.hour;
  local.tm_min = value.minute;
  local.tm_sec = value.second;
  local.tm_isdst = -1;
  return static_cast<long long>(std::mktime(&local));
}
#endif

} // namespace

bool for_each_dir_entry(const std::string &path,
                        const std::function<bool(const DirEntryInfo &)> &fn,
                        const char *diag_label) {
  DIR *directory = opendir(path.c_str());
  if (!directory) {
    return false;
  }
  if (diag_label != nullptr && diag_enabled()) {
    diag_log(std::string("      ") + diag_label + " dir " + path);
  }
  long long listed = 0;
  bool keep_going = true;
  while (keep_going) {
    dirent *entry = readdir(directory);
    if (!entry) {
      break;
    }
    if (is_dot_entry(entry->d_name)) {
      continue;
    }
    ++listed;
    if (diag_label != nullptr && diag_enabled() && diag_should_log_count(listed)) {
      diag_log(std::string("      ") + diag_label + " dir " + path +
               " still listing: " + std::to_string(listed) + " entries");
    }
    DirEntryInfo info {};
    info.name = entry->d_name;
#ifdef __vita__
    info.stat_ok = true;
    info.is_directory = SCE_S_ISDIR(entry->d_stat.st_mode);
    info.is_regular = SCE_S_ISREG(entry->d_stat.st_mode);
    if (info.is_regular) {
      info.size = static_cast<long long>(entry->d_stat.st_size);
      info.mtime = sce_datetime_to_epoch(entry->d_stat.st_mtime);
    }
#else
    struct stat st {};
    if (stat(join_path(path, entry->d_name).c_str(), &st) == 0) {
      info.stat_ok = true;
      info.is_directory = S_ISDIR(st.st_mode);
      info.is_regular = S_ISREG(st.st_mode);
      if (info.is_regular) {
        info.size = static_cast<long long>(st.st_size);
        info.mtime = static_cast<long long>(st.st_mtime);
      }
    }
#endif
    keep_going = fn(info);
  }
  closedir(directory);
  return true;
}

} // namespace vsm
