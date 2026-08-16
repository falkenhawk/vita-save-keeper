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
  static_assert(sizeof(ArchiveEntryInfo{}.size) == 4,
                "the %u/%08x formats assume 32-bit entry fields");

  std::vector<ArchiveEntryInfo> sorted = entries;
  std::sort(sorted.begin(), sorted.end(),
            [](const ArchiveEntryInfo &a, const ArchiveEntryInfo &b) { return a.path < b.path; });

  std::uint64_t hash = 14695981039346656037ULL;
  char buffer[32];
  for (const ArchiveEntryInfo &entry : sorted) {
    // the + 1 feeds each buffer's NUL - that is where the spec's '\0' separators come from.
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

std::vector<ArchiveEntryInfo> comparison_entries(const std::vector<ArchiveEntryInfo> &entries) {
  // The plain-content marker is deliberately NOT filtered here, unlike an earlier draft of this
  // helper: entries_match_backup_archive's whole plain-vs-raw safety net depends on the marker
  // making a plain archive's entry count refuse to match an unmounted folder walk (see
  // test_backup_archive_plain_marker_written_first_and_breaks_cd_match's comment) - stripping it
  // on both sides would make a plain archive falsely "match" a raw walk that happens to share the
  // same real files, which is exactly the class of mistake plan_backup_creation's sidecar-triple
  // special case exists to avoid. A live folder walk never produces the marker anyway, so leaving
  // it unfiltered costs nothing on the folder side; only an archive's own central directory ever
  // carries it, and that is precisely where a comparison must keep noticing it.
  std::vector<ArchiveEntryInfo> filtered;
  filtered.reserve(entries.size());
  for (const ArchiveEntryInfo &entry : entries) {
    const bool is_pfs_bookkeeping =
        entry.path == "sce_pfs" || entry.path.compare(0, 8, "sce_pfs/") == 0;
    if (is_pfs_bookkeeping) {
      continue;
    }
    filtered.push_back(entry);
  }
  return filtered;
}

} // namespace vsm
