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

// Entries relevant to a content SIGNATURE or entries-match COMPARISON: comparisons and signatures
// cover GAME FILES ONLY. Strips:
//  - sce_pfs/ - PFS's own bookkeeping; the app's held-mount metadata reads dirty it between
//    sessions independent of whether the save itself changed, which is why an unfiltered signature
//    almost never repeats for a retail save.
//  - sce_sys/ - its raw bytes are rewritten/re-encrypted by the console independent of meaningful
//    save changes (same noise class as sce_pfs above), so including it made "no changes since"
//    almost never fire either. This trades a narrow false-unchanged window (a save differing only
//    in sce_sys scratch, e.g. safemem.dat, reads as unchanged - the "Create New Backup Anyway"
//    escape hatch covers it) for the check meaning what users expect. Archive CONTENT is
//    unaffected: sce_sys is still fully captured and restored, just not compared.
//  - kRawSkeletonPrefix (".raw/") - Amendment B's raw sce_sys/sce_pfs skeleton payload, restore
//    bookkeeping rather than savedata a live folder walk could ever produce.
// Apply this to BOTH sides of any comparison - see entries_match_backup_archive, which filters its
// archive-read entries with this too, not just its folder-side caller (an OLD raw archive's
// central directory still lists sce_pfs/sce_sys plainly, since the writer never excluded them -
// only comparisons do - so filtering both sides keeps such an archive matchable against today's
// filtered folder walk instead of permanently unmatchable). The plain-content marker
// (kPlainContentMarkerName) and format entry (kPlainFormatEntryName) are deliberately left
// UNfiltered: either one is what makes a plain-content archive's entries refuse to match an
// unmounted folder walk by count, which is exactly the signal plan_backup_creation's
// sidecar-triple special case relies on existing (see this function's own .cpp comment for the
// test that pins this). The ARCHIVE ITSELF is unaffected by any of this: a raw archive's central
// directory still lists sce_pfs/sce_sys and restore still needs those bytes back - only
// comparisons ignore them.
std::vector<ArchiveEntryInfo> comparison_entries(const std::vector<ArchiveEntryInfo> &entries);

} // namespace vsm
