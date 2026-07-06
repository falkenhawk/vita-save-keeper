#include "core/BackupList.hpp"

#include "core/BackupName.hpp"

#include <algorithm>

namespace vsm {

BackupRow BackupRow::new_backup_row() {
  BackupRow row;
  row.new_backup = true;
  return row;
}

std::string BackupRow::display_name() const {
  if (new_backup) {
    return "New Backup";
  }
  // Only the display is transformed (no ".zip", the " auto" marker shown as an [AUTO] prefix) -
  // the stored file name is never mutated. Cloud presence is drawn as a pill by the UI instead
  // of a "[GD] " text prefix, so both sides render the same name.
  const std::string plain = vsm::display_backup_name(primary_name());
  constexpr const char *kAutoSuffix = " auto";
  const std::size_t suffix_length = 5;
  if (plain.size() > suffix_length &&
      plain.compare(plain.size() - suffix_length, suffix_length, kAutoSuffix) == 0) {
    return "[AUTO] " + plain.substr(0, plain.size() - suffix_length);
  }
  return plain;
}

std::vector<BackupRow> build_backup_rows(const std::vector<std::string> &remote_files,
                                         const std::vector<std::string> &local_files) {
  std::vector<BackupRow> snapshots;
  snapshots.reserve(remote_files.size() + local_files.size());

  for (const std::string &file : local_files) {
    BackupRow row;
    row.local_name = file;
    snapshots.push_back(std::move(row));
  }

  // A Drive copy attaches to the first identity match still missing its remote side; the guard
  // keeps two same-identity remote files (possible after out-of-band Drive edits) from
  // collapsing into one row and hiding a file.
  for (const std::string &file : remote_files) {
    const std::string identity = backup_identity(file);
    bool attached = false;
    for (BackupRow &row : snapshots) {
      if (!row.has_remote() && row.has_local() && backup_identity(row.local_name) == identity) {
        row.remote_name = file;
        attached = true;
        break;
      }
    }
    if (!attached) {
      BackupRow row;
      row.remote_name = file;
      snapshots.push_back(std::move(row));
    }
  }

  // Newest first regardless of which side holds the snapshot; the timestamp identity keeps the
  // order chronological even after a label rename. Same-identity rows (foreign names, rare
  // duplicates) tiebreak on the full name so the order stays stable.
  std::sort(snapshots.begin(), snapshots.end(), [](const BackupRow &a, const BackupRow &b) {
    const std::string identity_a = backup_identity(a.primary_name());
    const std::string identity_b = backup_identity(b.primary_name());
    if (identity_a != identity_b) {
      return identity_a > identity_b;
    }
    return a.primary_name() > b.primary_name();
  });

  std::vector<BackupRow> result;
  result.reserve(1 + snapshots.size());
  result.push_back(BackupRow::new_backup_row());
  for (BackupRow &row : snapshots) {
    result.push_back(std::move(row));
  }
  return result;
}

} // namespace vsm
