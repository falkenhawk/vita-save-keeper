#pragma once

#include <functional>
#include <string>

namespace vsm {

// Hard ceiling on entries visited per save-folder walk. Real savedata tops out around a hundred
// files - the largest observed on a fully loaded card is 110 files, roughly 250 entries once
// sce_sys and the sce_pfs integrity mirror are counted, and big-save games (LBP, DQB) are a few
// large files rather than many small ones. Homebrew data hoards sit orders of magnitude above:
// the issue #7 folder carried thousands of files in its cache. Forty times the observed maximum
// keeps every legitimate save far inside the limit and still bounds a pathological walk to
// about a second.
constexpr long long kMaxSaveWalkEntries = 10000;

struct DirEntryInfo {
  const char *name;
  // false when the entry's metadata could not be read; callers treat that as an unreadable
  // entry, distinct from a readable special file which is neither directory nor regular
  bool stat_ok;
  bool is_directory;
  bool is_regular;
  long long size;   // regular files only
  long long mtime;  // epoch seconds, regular files only
};

// Visits every direct entry of path except . and .. and hands fn a stat-equivalent record. On
// the Vita the record comes straight from readdir's d_stat - one syscall per directory instead
// of one per entry. That difference is what froze issue #7's boot: a path-based stat rescans
// the FAT directory chain per lookup, so statting every entry of an N-entry directory is
// O(N^2), and a several-thousand-file folder takes minutes to hours. Everywhere else (desktop
// tests) it falls back to one stat() per entry.
// fn returning false stops the listing early. Returns false when path could not be opened as a
// directory. diag_label, when set, writes "<label> dir <path>" and sparse still-listing lines
// to the diagnostic trace (see DiagTrace.hpp).
bool for_each_dir_entry(const std::string &path,
                        const std::function<bool(const DirEntryInfo &)> &fn,
                        const char *diag_label = nullptr);

} // namespace vsm
