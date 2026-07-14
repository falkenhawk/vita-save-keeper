#include "core/SaveSlotMetadata.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace vsm {
namespace {

constexpr std::size_t kActiveSlotsOffset = 0x200;
constexpr std::size_t kTitleOffset = 0x04;
constexpr std::size_t kTitleSize = 0x40;
constexpr std::size_t kSubtitleOffset = 0x44;
constexpr std::size_t kSubtitleSize = 0x80;
constexpr std::size_t kDetailsOffset = 0xc4;
constexpr std::size_t kDetailsSize = 0x200;
constexpr std::size_t kDateOffset = 0x30c;
constexpr std::uint32_t kSdslotMagic = 0x4c534453;

std::uint16_t read_le16(const std::vector<unsigned char> &data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(data[offset + 1] << 8);
}

std::uint32_t read_le32(const std::vector<unsigned char> &data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

bool is_leap_year(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool valid_datetime(const SaveDateTime &value) {
  if (value.year < 1 || value.year > 9999 || value.month < 1 || value.month > 12 ||
      value.hour < 0 || value.hour > 23 || value.minute < 0 || value.minute > 59 ||
      value.second < 0 || value.second > 59) {
    return false;
  }
  static constexpr std::array<int, 12> kMonthDays = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
  };
  int days = kMonthDays[static_cast<std::size_t>(value.month - 1)];
  if (value.month == 2 && is_leap_year(value.year)) {
    ++days;
  }
  return value.day >= 1 && value.day <= days;
}

bool datetime_less(const SaveDateTime &left, const SaveDateTime &right) {
  const int left_values[] = {left.year, left.month, left.day, left.hour, left.minute, left.second};
  const int right_values[] = {right.year, right.month, right.day, right.hour, right.minute,
                              right.second};
  return std::lexicographical_compare(std::begin(left_values), std::end(left_values),
                                      std::begin(right_values), std::end(right_values));
}

void append_replacement(std::string *out) {
  out->append("\xef\xbf\xbd", 3);
}

std::string sanitize_utf8(const unsigned char *bytes, std::size_t size) {
  std::string out;
  out.reserve(size);
  std::size_t i = 0;
  while (i < size) {
    const unsigned char lead = bytes[i];
    if (lead < 0x80) {
      out.push_back(static_cast<char>(lead));
      ++i;
      continue;
    }

    std::size_t length = 0;
    std::uint32_t codepoint = 0;
    std::uint32_t minimum = 0;
    if (lead >= 0xc2 && lead <= 0xdf) {
      length = 2;
      codepoint = lead & 0x1f;
      minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      length = 3;
      codepoint = lead & 0x0f;
      minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      length = 4;
      codepoint = lead & 0x07;
      minimum = 0x10000;
    } else {
      append_replacement(&out);
      ++i;
      continue;
    }

    bool valid = i + length <= size;
    for (std::size_t j = 1; valid && j < length; ++j) {
      const unsigned char continuation = bytes[i + j];
      if ((continuation & 0xc0) != 0x80) {
        valid = false;
      } else {
        codepoint = (codepoint << 6) | (continuation & 0x3f);
      }
    }
    valid = valid && codepoint >= minimum && codepoint <= 0x10ffff &&
            !(codepoint >= 0xd800 && codepoint <= 0xdfff);
    if (!valid) {
      append_replacement(&out);
      ++i;
      continue;
    }
    out.append(reinterpret_cast<const char *>(bytes + i), length);
    i += length;
  }
  return out;
}

bool read_fixed_string(const std::vector<unsigned char> &data, std::size_t offset,
                       std::size_t capacity, std::string *value) {
  const auto begin = data.begin() + static_cast<long>(offset);
  const auto end = begin + static_cast<long>(capacity);
  const auto terminator = std::find(begin, end, 0);
  if (terminator == end) {
    return false;
  }
  const std::size_t length = static_cast<std::size_t>(terminator - begin);
  *value = sanitize_utf8(data.data() + offset, length);
  return true;
}

std::string join_path(const std::string &parent, const std::string &child) {
  if (parent.empty() || parent.back() == '/') {
    return parent + child;
  }
  return parent + "/" + child;
}

bool is_dot_entry(const char *name) {
  return std::string(name) == "." || std::string(name) == "..";
}

void find_newest_file_mtime(const std::string &path, std::time_t *newest, bool *found) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    return;
  }
  if (S_ISREG(info.st_mode)) {
    if (!*found || info.st_mtime > *newest) {
      *newest = info.st_mtime;
      *found = true;
    }
    return;
  }
  if (!S_ISDIR(info.st_mode)) {
    return;
  }

  DIR *directory = opendir(path.c_str());
  if (!directory) {
    return;
  }
  while (dirent *entry = readdir(directory)) {
    if (!is_dot_entry(entry->d_name)) {
      find_newest_file_mtime(join_path(path, entry->d_name), newest, found);
    }
  }
  closedir(directory);
}

bool read_sdslot_file(const std::string &path, std::vector<unsigned char> *data) {
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return false;
  }
  constexpr std::size_t kMaximumSize = kSdslotHeaderSize + kMaxSaveSlots * kSdslotRecordSize;
  data->clear();
  std::array<unsigned char, 4096> buffer {};
  bool ok = true;
  while (true) {
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
    if (read > 0) {
      if (data->size() + read > kMaximumSize) {
        ok = false;
        break;
      }
      data->insert(data->end(), buffer.begin(), buffer.begin() + static_cast<long>(read));
    }
    if (read < buffer.size()) {
      ok = std::ferror(file) == 0;
      break;
    }
  }
  std::fclose(file);
  return ok;
}

SaveDateTime datetime_from_time(std::time_t value) {
  const std::tm *local = std::localtime(&value);
  if (!local) {
    return {};
  }
  return {local->tm_year + 1900, local->tm_mon + 1, local->tm_mday, local->tm_hour,
          local->tm_min, local->tm_sec};
}

} // namespace

SaveMetadata parse_sdslot_data(const std::vector<unsigned char> &data) {
  SaveMetadata result;
  if (data.size() < kSdslotHeaderSize || read_le32(data, 0) != kSdslotMagic) {
    return result;
  }

  for (std::size_t slot_id = 0; slot_id < kMaxSaveSlots; ++slot_id) {
    if (data[kActiveSlotsOffset + slot_id] == 0) {
      continue;
    }
    const std::size_t record = kSdslotHeaderSize + slot_id * kSdslotRecordSize;
    if (record + kSdslotRecordSize > data.size()) {
      continue;
    }

    SaveSlotMetadata slot;
    slot.id = static_cast<unsigned int>(slot_id);
    slot.modified_at = {
        read_le16(data, record + kDateOffset),
        read_le16(data, record + kDateOffset + 2),
        read_le16(data, record + kDateOffset + 4),
        read_le16(data, record + kDateOffset + 6),
        read_le16(data, record + kDateOffset + 8),
        read_le16(data, record + kDateOffset + 10),
    };
    if (!valid_datetime(slot.modified_at) ||
        !read_fixed_string(data, record + kTitleOffset, kTitleSize, &slot.title) ||
        !read_fixed_string(data, record + kSubtitleOffset, kSubtitleSize, &slot.subtitle) ||
        !read_fixed_string(data, record + kDetailsOffset, kDetailsSize, &slot.details)) {
      continue;
    }
    result.slots.push_back(std::move(slot));
  }

  if (!result.slots.empty()) {
    result.source = SaveTimeSource::VitaSlot;
    result.approximate = false;
    result.saved_at = result.slots.front().modified_at;
    for (const SaveSlotMetadata &slot : result.slots) {
      if (datetime_less(result.saved_at, slot.modified_at)) {
        result.saved_at = slot.modified_at;
      }
    }
  }
  return result;
}

SaveMetadata resolve_save_metadata(const std::string &save_path,
                                   const SaveDateTime &backup_clock) {
  std::vector<unsigned char> sdslot;
  if (read_sdslot_file(join_path(join_path(save_path, "sce_sys"), "sdslot.dat"), &sdslot)) {
    SaveMetadata parsed = parse_sdslot_data(sdslot);
    if (!parsed.slots.empty()) {
      return parsed;
    }
  }

  std::time_t newest = 0;
  bool found = false;
  find_newest_file_mtime(save_path, &newest, &found);
  SaveMetadata result;
  if (found) {
    result.saved_at = datetime_from_time(newest);
    result.source = SaveTimeSource::Filesystem;
    result.approximate = true;
  } else {
    result.saved_at = backup_clock;
    result.source = SaveTimeSource::BackupClock;
    result.approximate = true;
  }
  return result;
}

SaveDateTime current_local_datetime() {
  return datetime_from_time(std::time(nullptr));
}

std::string format_save_datetime(const SaveDateTime &value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d", value.year,
                value.month, value.day, value.hour, value.minute, value.second);
  return buffer;
}

long long save_datetime_to_local_epoch(const SaveDateTime &value) {
  std::tm local {};
  local.tm_year = value.year - 1900;
  local.tm_mon = value.month - 1;
  local.tm_mday = value.day;
  local.tm_hour = value.hour;
  local.tm_min = value.minute;
  local.tm_sec = value.second;
  local.tm_isdst = -1;
  return static_cast<long long>(std::mktime(&local));
}

} // namespace vsm
