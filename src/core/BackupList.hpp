#pragma once

#include <string>
#include <vector>

namespace vsm {

// One row of the backup picker: a snapshot identified by its timestamp prefix, present on the
// card, on Drive, or both. Index 0 of the built list is the "New Backup" sentinel.
struct BackupRow {
  // File names including ".zip"; empty when that side does not hold the snapshot. After a rename
  // that only reached one side the two names can differ - they still pair by identity.
  std::string local_name;
  std::string remote_name;
  bool new_backup{};

  static BackupRow new_backup_row();

  bool has_local() const { return !local_name.empty(); }
  bool has_remote() const { return !remote_name.empty(); }
  // The name actions operate on; the card copy wins when both sides exist because local renames
  // apply first and a stale Drive name heals on the next rename.
  const std::string &primary_name() const { return has_local() ? local_name : remote_name; }

  std::string display_name() const;
};

// Merges the two per-save lists into one row per snapshot, paired by backup_identity, newest
// first. "New Backup" stays at index 0.
std::vector<BackupRow> build_backup_rows(const std::vector<std::string> &remote_files,
                                         const std::vector<std::string> &local_files);

// Details may offer an explicit full-ZIP download only when the backup exists solely on Drive
// and its small JSON companion could not provide usable metadata.
bool backup_details_download_available(const BackupRow &row, bool usable_metadata);

} // namespace vsm
