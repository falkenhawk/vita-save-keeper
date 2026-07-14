#pragma once

#include <cstddef>
#include <string>

namespace vsm {

struct BackupTimestamp {
  int year{};
  int month{};
  int day{};
  int hour{};
  int minute{};
  int second{};
};

// "YYYY-MM-DD HH-MM-SS" - the identity prefix every backup name starts with.
constexpr std::size_t kBackupTimestampPrefixLength = 19;
// Longest user label kept after sanitizing, counted in Unicode codepoints (characters), not
// bytes. This is a readability choice, not a hard limit: the full name is only
// "YYYY-MM-DD HH-MM-SS <label>.zip", so even at this length it stays well under the ~255-char
// exFAT filename limit and far under Drive's. The IME caps typing at the same character count,
// so a multi-byte (CJK) label is never silently cut below what the keyboard let the user enter.
constexpr std::size_t kMaxBackupLabelLength = 60;

std::string make_timestamped_backup_name(const BackupTimestamp &timestamp);
// UI form of a backup file name: the ".zip" extension is an on-disk detail every backup shares,
// so lists and status messages drop it.
std::string display_backup_name(const std::string &file_name);

// True when the ".zip"-stripped name starts with the digit/separator timestamp pattern and the
// prefix is followed by end-of-name or a space. Foreign zips that merely share 19 leading chars
// must not pass, or unrelated files would merge into one row.
bool has_backup_timestamp_prefix(const std::string &file_name);
// What pairs a local file with its Drive copy across renames: the timestamp prefix when the
// pattern holds, otherwise the whole stripped name (no accidental merging of foreign files).
std::string backup_identity(const std::string &file_name);
// Identity-based optional metadata companion name. Labels never affect it.
std::string backup_metadata_name(const std::string &file_name);
// Text after the timestamp prefix, "" when none. The " auto" marker reads as the label "auto".
std::string backup_label(const std::string &file_name);
// Rebuilds the file name keeping the identity: "<identity> <label>.zip", or "<identity>.zip"
// when the label is empty. The caller must pass a name with a valid timestamp prefix.
std::string backup_name_with_label(const std::string &file_name, const std::string &label);
// Filesystem- and Drive-safe label: unsafe path characters and control bytes become '_',
// whitespace runs collapse to a single space, trimmed, capped at kMaxBackupLabelLength bytes on
// a UTF-8 codepoint boundary. An empty result means "no label".
std::string sanitize_backup_label(const std::string &raw_label);
// A label ending in the word "auto" would make the renamed file indistinguishable from an
// automatic pre-restore snapshot; such labels are rejected at edit time.
bool backup_label_conflicts_with_auto(const std::string &label);

} // namespace vsm
