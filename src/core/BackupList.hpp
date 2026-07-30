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
  // The homebrew entry's "Savedata Paths" row: a way into the path picker that lives in the list
  // rather than behind a button chord. Carries no file names and is never a backup.
  bool data_folders{};
  // How many extra folders the entry has, for this row's label. Meaningless on any other row.
  std::size_t data_folder_count{};
  // Replaces "New Backup" for a cloud-only entry: there is no save on this console to back up,
  // and the row says so where the user would look for the backup action. Focusable but inert.
  bool no_live_save{};

  static BackupRow new_backup_row();
  static BackupRow data_folders_row(std::size_t count);
  static BackupRow no_live_save_row();

  // Rows that stand for an action rather than a backup. Every backup operation - restore, delete,
  // label, transfer - must refuse these, which is why the accessors return them as "no row".
  bool is_sentinel() const { return new_backup || data_folders || no_live_save; }

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

} // namespace vsm
