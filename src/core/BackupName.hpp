#pragma once

#include <string>

namespace vsm {

struct BackupTimestamp {
  int year{};
  int month{};
  int day{};
  int hour{};
  int minute{};
};

std::string make_timestamped_backup_name(const BackupTimestamp &timestamp);

} // namespace vsm
