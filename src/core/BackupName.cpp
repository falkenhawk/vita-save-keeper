#include "core/BackupName.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace vsm {
namespace {

// Same set normalize_path_component treats as unsafe: labels become part of local file names and
// Drive object names, so the two sanitizers must agree on what can appear in a path component.
bool is_unsafe_label_character(char value) {
  switch (value) {
  case '\\':
  case '/':
  case ':':
  case '*':
  case '?':
  case '"':
  case '\'':
  case '<':
  case '>':
  case '|':
    return true;
  default:
    return false;
  }
}

bool is_digit_at(const std::string &text, std::size_t index) {
  return std::isdigit(static_cast<unsigned char>(text[index])) != 0;
}

std::size_t canonical_identity_length(const std::string &plain) {
  if (plain.size() < kBackupTimestampPrefixLength) {
    return 0;
  }
  for (std::size_t i = 0; i < kBackupTimestampPrefixLength; ++i) {
    const bool separator_position = i == 4 || i == 7 || i == 10 || i == 13 || i == 16;
    if (separator_position) {
      const char expected = i == 10 ? ' ' : '-';
      if (plain[i] != expected) {
        return 0;
      }
    } else if (!is_digit_at(plain, i)) {
      return 0;
    }
  }

  std::size_t length = kBackupTimestampPrefixLength;
  if (length < plain.size() && plain[length] == '~') {
    const std::size_t digits_start = ++length;
    while (length < plain.size() && is_digit_at(plain, length)) {
      ++length;
    }
    if (length == digits_start) {
      return 0;
    }
    unsigned long counter = 0;
    for (std::size_t i = digits_start; i < length; ++i) {
      counter = counter * 10 + static_cast<unsigned long>(plain[i] - '0');
      if (counter > 999999UL) {
        return 0;
      }
    }
    if (counter < 2) {
      return 0;
    }
  }
  return length == plain.size() || plain[length] == ' ' ? length : 0;
}

} // namespace

std::string make_timestamped_backup_name(const BackupTimestamp &timestamp) {
  // Use JKSV-style snapshot names rather than a single mutable "latest" file. The timestamp is
  // readable on-device, sorts chronologically when zero-padded, and lets Drive hold multiple
  // restore points without inventing a conflict resolver. Seconds are included so several
  // backups created within the same minute do not collide on the file name.
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << timestamp.year << "-" << std::setw(2)
      << timestamp.month << "-" << std::setw(2) << timestamp.day << " " << std::setw(2)
      << timestamp.hour << "-" << std::setw(2) << timestamp.minute << "-" << std::setw(2)
      << timestamp.second << ".zip";
  return out.str();
}

std::string display_backup_name(const std::string &file_name) {
  constexpr const char *kZipExtension = ".zip";
  constexpr std::size_t kZipExtensionLength = 4;
  if (file_name.size() >= kZipExtensionLength &&
      file_name.compare(file_name.size() - kZipExtensionLength, kZipExtensionLength,
                        kZipExtension) == 0) {
    return file_name.substr(0, file_name.size() - kZipExtensionLength);
  }
  return file_name;
}

bool has_backup_timestamp_prefix(const std::string &file_name) {
  const std::string plain = display_backup_name(file_name);
  return canonical_identity_length(plain) != 0;
}

std::string backup_identity(const std::string &file_name) {
  const std::string plain = display_backup_name(file_name);
  if (has_backup_timestamp_prefix(file_name)) {
    return plain.substr(0, canonical_identity_length(plain));
  }
  return plain;
}

std::string backup_metadata_name(const std::string &file_name) {
  return backup_identity(file_name) + ".json";
}

std::string backup_label(const std::string &file_name) {
  if (!has_backup_timestamp_prefix(file_name)) {
    return {};
  }
  const std::string plain = display_backup_name(file_name);
  const std::size_t identity_length = canonical_identity_length(plain);
  if (plain.size() <= identity_length + 1) {
    return {};
  }
  return plain.substr(identity_length + 1);
}

std::string backup_name_with_label(const std::string &file_name, const std::string &label) {
  std::string result = backup_identity(file_name);
  if (!label.empty()) {
    result += " ";
    result += label;
  }
  result += ".zip";
  return result;
}

std::string sanitize_backup_label(const std::string &raw_label) {
  std::string collapsed;
  collapsed.reserve(raw_label.size());
  bool pending_space = false;
  for (char value : raw_label) {
    const unsigned char byte = static_cast<unsigned char>(value);
    // Multi-byte UTF-8 sequences (byte >= 0x80) pass through untouched; the checks below only
    // target ASCII controls, ASCII whitespace, and the unsafe path set.
    if (byte < 0x80 && (byte < 0x20 || byte == 0x7f)) {
      continue;
    }
    char sanitized = value;
    if (byte < 0x80 && is_unsafe_label_character(value)) {
      sanitized = '_';
    }
    if (sanitized == ' ') {
      pending_space = !collapsed.empty();
      continue;
    }
    if (pending_space) {
      collapsed += ' ';
      pending_space = false;
    }
    collapsed += sanitized;
  }

  // Cap by codepoint count so the limit matches the IME's character limit; a lead byte is the
  // start of a codepoint, continuation bytes (0x80 pattern) belong to the same one. Truncating on
  // a lead byte never splits a UTF-8 sequence.
  std::size_t codepoints = 0;
  std::size_t cut = collapsed.size();
  for (std::size_t i = 0; i < collapsed.size(); ++i) {
    if ((static_cast<unsigned char>(collapsed[i]) & 0xC0) == 0x80) {
      continue;
    }
    if (codepoints == kMaxBackupLabelLength) {
      cut = i;
      break;
    }
    ++codepoints;
  }
  if (cut < collapsed.size()) {
    collapsed.erase(cut);
    // The cap can expose a space that used to sit mid-label.
    while (!collapsed.empty() && collapsed.back() == ' ') {
      collapsed.pop_back();
    }
  }
  return collapsed;
}

bool backup_label_conflicts_with_auto(const std::string &label) {
  constexpr const char *kAuto = "auto";
  constexpr std::size_t kAutoLength = 4;
  if (label == kAuto) {
    return true;
  }
  return label.size() > kAutoLength &&
         label.compare(label.size() - kAutoLength - 1, kAutoLength + 1, " auto") == 0;
}

} // namespace vsm
