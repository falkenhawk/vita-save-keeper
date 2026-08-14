#include "core/ContentSignature.hpp"

#include <algorithm>
#include <cstdio>

namespace vsm {
namespace {

void fnv1a64_update(std::uint64_t *hash, const char *data, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    *hash ^= static_cast<unsigned char>(data[i]);
    *hash *= 1099511628211ULL;
  }
}

} // namespace

std::string compute_content_signature(const std::vector<ArchiveEntryInfo> &entries) {
  std::vector<ArchiveEntryInfo> sorted = entries;
  std::sort(sorted.begin(), sorted.end(),
            [](const ArchiveEntryInfo &a, const ArchiveEntryInfo &b) { return a.path < b.path; });

  std::uint64_t hash = 14695981039346656037ULL;
  char buffer[32];
  for (const ArchiveEntryInfo &entry : sorted) {
    fnv1a64_update(&hash, entry.path.data(), entry.path.size() + 1);
    const int crc_length = std::snprintf(buffer, sizeof(buffer), "%08x", entry.crc32);
    fnv1a64_update(&hash, buffer, static_cast<std::size_t>(crc_length) + 1);
    const int size_length = std::snprintf(buffer, sizeof(buffer), "%u\n", entry.size);
    fnv1a64_update(&hash, buffer, static_cast<std::size_t>(size_length));
  }

  char rendered[17];
  std::snprintf(rendered, sizeof(rendered), "%016llx", static_cast<unsigned long long>(hash));
  return rendered;
}

ContentTotals compute_content_totals(const std::vector<ArchiveEntryInfo> &entries) {
  ContentTotals totals;
  for (const ArchiveEntryInfo &entry : entries) {
    totals.total_bytes += entry.size;
  }
  totals.file_count = entries.size();
  return totals;
}

} // namespace vsm
