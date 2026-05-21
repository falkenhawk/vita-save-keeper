#include "core/BackupName.hpp"

#include <iomanip>
#include <sstream>

namespace vsm {

std::string make_timestamped_backup_name(const BackupTimestamp &timestamp) {
  // Use JKSV-style snapshot names rather than a single mutable "latest" file. The timestamp is
  // readable on-device, sorts chronologically when zero-padded, and lets Drive hold multiple
  // restore points without inventing a conflict resolver.
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << timestamp.year << "-" << std::setw(2)
      << timestamp.month << "-" << std::setw(2) << timestamp.day << " " << std::setw(2)
      << timestamp.hour << "-" << std::setw(2) << timestamp.minute << ".zip";
  return out.str();
}

} // namespace vsm
