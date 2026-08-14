#pragma once

#include "core/BackupArchive.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vsm {

struct ContentTotals {
  std::uint64_t total_bytes{};
  std::uint64_t file_count{};
};

// Content signature of a backup: FNV-1a 64 over the entries sorted by path, each fed as
// path + '\0' + lowercase 8-hex crc32 + '\0' + decimal size + '\n', rendered as 16 lowercase hex
// digits. Byte-exact and sorted so every device derives the same value for the same content; a
// match therefore also implies equal file count and total size. Change detection, not security -
// the same trust level as comparing a local archive's central directory.
std::string compute_content_signature(const std::vector<ArchiveEntryInfo> &entries);
ContentTotals compute_content_totals(const std::vector<ArchiveEntryInfo> &entries);

} // namespace vsm
