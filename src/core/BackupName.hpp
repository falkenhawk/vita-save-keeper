#pragma once

#include <string>

namespace vsm {

struct BackupTimestamp {
  int year{};
  int month{};
  int day{};
  int hour{};
  int minute{};
  int second{};
};

std::string make_timestamped_backup_name(const BackupTimestamp &timestamp);
// UI form of a backup file name: the ".zip" extension is an on-disk detail every backup shares,
// so lists and status messages drop it.
std::string display_backup_name(const std::string &file_name);

} // namespace vsm
