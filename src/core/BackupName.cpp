#include "core/BackupName.hpp"

#include <iomanip>
#include <sstream>

namespace vsm {

std::string make_timestamped_backup_name(const BackupTimestamp &timestamp) {
  // Use JKSV-style snapshot names rather than a single mutable "latest" file. The timestamp is
  // readable on-device, sorts chronologically when zero-padded, and lets Drive hold multiple
  // restore points without inventing a conflict resolver. Seconds are included so several
  // backups created within the same minute do not collide on the file name.
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << timestamp.year << "-" << std::setw(2)
      << timestamp.month << "-" << std::setw(2) << timestamp.day << " " << std::setw(2)
      << timestamp.hour << "-" << std::setw(2) << timestamp.minute << "-" << std::setw(2)
      << timestamp.second << ".zip";
  return out.str();
}

std::string display_backup_name(const std::string &file_name) {
  constexpr const char *kZipExtension = ".zip";
  constexpr std::size_t kZipExtensionLength = 4;
  if (file_name.size() >= kZipExtensionLength &&
      file_name.compare(file_name.size() - kZipExtensionLength, kZipExtensionLength,
                        kZipExtension) == 0) {
    return file_name.substr(0, file_name.size() - kZipExtensionLength);
  }
  return file_name;
}

} // namespace vsm
