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

// Entries relevant to a content SIGNATURE or entries-match COMPARISON: strips sce_pfs/ (PFS's own
// bookkeeping - the app's held-mount metadata reads dirty it between sessions independent of
// whether the save itself changed, which is why an unfiltered signature almost never repeats for
// a retail save). Apply this to BOTH sides of any comparison - see entries_match_backup_archive,
// which filters its archive-read entries with this too, not just its folder-side caller. The
// plain-content marker (kPlainContentMarkerName) is deliberately left UNfiltered: it is what makes
// a plain-content archive's entries refuse to match an unmounted folder walk, which is exactly the
// signal plan_backup_creation's sidecar-triple special case relies on existing (see this
// function's own .cpp comment for the test that pins this). The ARCHIVE ITSELF is unaffected by
// any of this: a raw archive's central directory still lists sce_pfs and restore still needs those
// bytes back - only comparisons ignore them.
std::vector<ArchiveEntryInfo> comparison_entries(const std::vector<ArchiveEntryInfo> &entries);

} // namespace vsm
