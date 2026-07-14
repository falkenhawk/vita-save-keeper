#include "core/SaveSlotMetadata.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
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

bool is_valid_utf8(const std::string &value) {
  return sanitize_utf8(reinterpret_cast<const unsigned char *>(value.data()), value.size()) == value;
}

void append_json_string(std::string *out, const std::string &value) {
  static constexpr char kHex[] = "0123456789abcdef";
  out->push_back('"');
  for (const unsigned char byte : value) {
    switch (byte) {
    case '"':
      *out += "\\\"";
      break;
    case '\\':
      *out += "\\\\";
      break;
    case '\b':
      *out += "\\b";
      break;
    case '\f':
      *out += "\\f";
      break;
    case '\n':
      *out += "\\n";
      break;
    case '\r':
      *out += "\\r";
      break;
    case '\t':
      *out += "\\t";
      break;
    default:
      if (byte < 0x20) {
        *out += "\\u00";
        out->push_back(kHex[byte >> 4]);
        out->push_back(kHex[byte & 0x0f]);
      } else {
        out->push_back(static_cast<char>(byte));
      }
    }
  }
  out->push_back('"');
}

void append_utf8_codepoint(std::string *out, std::uint32_t codepoint) {
  if (codepoint < 0x80) {
    out->push_back(static_cast<char>(codepoint));
  } else if (codepoint < 0x800) {
    out->push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else if (codepoint < 0x10000) {
    out->push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  } else {
    out->push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
  }
}

class JsonCursor {
public:
  explicit JsonCursor(const std::string &json) : json_(json) {}

  bool at_end() {
    skip_space();
    return position_ == json_.size();
  }

  bool consume(char expected) {
    skip_space();
    if (position_ >= json_.size() || json_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
  }

  bool parse_string(std::string *value) {
    skip_space();
    if (position_ >= json_.size() || json_[position_++] != '"') {
      return false;
    }
    value->clear();
    while (position_ < json_.size()) {
      const unsigned char byte = static_cast<unsigned char>(json_[position_++]);
      if (byte == '"') {
        return is_valid_utf8(*value);
      }
      if (byte < 0x20) {
        return false;
      }
      if (byte != '\\') {
        value->push_back(static_cast<char>(byte));
        continue;
      }
      if (position_ >= json_.size()) {
        return false;
      }
      const char escape = json_[position_++];
      switch (escape) {
      case '"': value->push_back('"'); break;
      case '\\': value->push_back('\\'); break;
      case '/': value->push_back('/'); break;
      case 'b': value->push_back('\b'); break;
      case 'f': value->push_back('\f'); break;
      case 'n': value->push_back('\n'); break;
      case 'r': value->push_back('\r'); break;
      case 't': value->push_back('\t'); break;
      case 'u': {
        std::uint32_t codepoint = 0;
        if (!parse_hex4(&codepoint)) {
          return false;
        }
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
          if (position_ + 2 > json_.size() || json_[position_] != '\\' ||
              json_[position_ + 1] != 'u') {
            return false;
          }
          position_ += 2;
          std::uint32_t low = 0;
          if (!parse_hex4(&low) || low < 0xdc00 || low > 0xdfff) {
            return false;
          }
          codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
          return false;
        }
        append_utf8_codepoint(value, codepoint);
        break;
      }
      default:
        return false;
      }
    }
    return false;
  }

  bool parse_unsigned(unsigned int *value) {
    skip_space();
    if (position_ >= json_.size() || !std::isdigit(static_cast<unsigned char>(json_[position_]))) {
      return false;
    }
    unsigned long long parsed = 0;
    while (position_ < json_.size() &&
           std::isdigit(static_cast<unsigned char>(json_[position_]))) {
      parsed = parsed * 10 + static_cast<unsigned int>(json_[position_] - '0');
      if (parsed > 0xffffffffULL) {
        return false;
      }
      ++position_;
    }
    *value = static_cast<unsigned int>(parsed);
    return true;
  }

  bool parse_bool(bool *value) {
    skip_space();
    if (json_.compare(position_, 4, "true") == 0) {
      position_ += 4;
      *value = true;
      return true;
    }
    if (json_.compare(position_, 5, "false") == 0) {
      position_ += 5;
      *value = false;
      return true;
    }
    return false;
  }

  bool skip_value(int depth = 0) {
    if (depth > 32) {
      return false;
    }
    skip_space();
    if (position_ >= json_.size()) {
      return false;
    }
    if (json_[position_] == '"') {
      std::string ignored;
      return parse_string(&ignored);
    }
    if (json_[position_] == '{') {
      ++position_;
      skip_space();
      if (consume('}')) {
        return true;
      }
      while (true) {
        std::string key;
        if (!parse_string(&key) || !consume(':') || !skip_value(depth + 1)) {
          return false;
        }
        if (consume('}')) {
          return true;
        }
        if (!consume(',')) {
          return false;
        }
      }
    }
    if (json_[position_] == '[') {
      ++position_;
      skip_space();
      if (consume(']')) {
        return true;
      }
      while (true) {
        if (!skip_value(depth + 1)) {
          return false;
        }
        if (consume(']')) {
          return true;
        }
        if (!consume(',')) {
          return false;
        }
      }
    }
    const std::size_t start = position_;
    while (position_ < json_.size() &&
           std::string(" \t\r\n,]}:").find(json_[position_]) == std::string::npos) {
      ++position_;
    }
    if (position_ == start) {
      return false;
    }
    const std::string token = json_.substr(start, position_ - start);
    if (token == "true" || token == "false" || token == "null") {
      return true;
    }
    std::size_t i = token[0] == '-' ? 1 : 0;
    bool digit = false;
    for (; i < token.size(); ++i) {
      const char ch = token[i];
      if (std::isdigit(static_cast<unsigned char>(ch))) {
        digit = true;
      } else if (ch != '.' && ch != 'e' && ch != 'E' && ch != '+' && ch != '-') {
        return false;
      }
    }
    return digit;
  }

private:
  void skip_space() {
    while (position_ < json_.size() &&
           std::isspace(static_cast<unsigned char>(json_[position_]))) {
      ++position_;
    }
  }

  bool parse_hex4(std::uint32_t *value) {
    if (position_ + 4 > json_.size()) {
      return false;
    }
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = json_[position_++];
      result <<= 4;
      if (ch >= '0' && ch <= '9') result |= static_cast<std::uint32_t>(ch - '0');
      else if (ch >= 'a' && ch <= 'f') result |= static_cast<std::uint32_t>(ch - 'a' + 10);
      else if (ch >= 'A' && ch <= 'F') result |= static_cast<std::uint32_t>(ch - 'A' + 10);
      else return false;
    }
    *value = result;
    return true;
  }

  const std::string &json_;
  std::size_t position_{};
};

bool parse_datetime_string(const std::string &text, SaveDateTime *value) {
  if (text.size() != 19 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
      text[13] != ':' || text[16] != ':') {
    return false;
  }
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
    if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
  }
  const auto number = [&](std::size_t offset, std::size_t length) {
    int result = 0;
    for (std::size_t i = 0; i < length; ++i) result = result * 10 + text[offset + i] - '0';
    return result;
  };
  *value = {number(0, 4), number(5, 2), number(8, 2), number(11, 2), number(14, 2),
            number(17, 2)};
  return valid_datetime(*value);
}

bool parse_slot_json(JsonCursor *cursor, SaveSlotMetadata *slot) {
  if (!cursor->consume('{')) return false;
  bool have_id = false, have_time = false, have_title = false, have_subtitle = false,
       have_details = false;
  if (cursor->consume('}')) return false;
  while (true) {
    std::string key;
    if (!cursor->parse_string(&key) || !cursor->consume(':')) return false;
    if (key == "id") {
      have_id = cursor->parse_unsigned(&slot->id) && slot->id < kMaxSaveSlots;
      if (!have_id) return false;
    } else if (key == "modifiedAt") {
      std::string text;
      have_time = cursor->parse_string(&text) && parse_datetime_string(text, &slot->modified_at);
      if (!have_time) return false;
    } else if (key == "title" || key == "subtitle" || key == "details") {
      std::string *target = key == "title" ? &slot->title :
                            key == "subtitle" ? &slot->subtitle : &slot->details;
      const std::size_t limit = key == "title" ? kTitleSize :
                                key == "subtitle" ? kSubtitleSize : kDetailsSize;
      if (!cursor->parse_string(target) || target->size() > limit) return false;
      if (key == "title") have_title = true;
      else if (key == "subtitle") have_subtitle = true;
      else have_details = true;
    } else if (!cursor->skip_value(1)) {
      return false;
    }
    if (cursor->consume('}')) break;
    if (!cursor->consume(',')) return false;
  }
  return have_id && have_time && have_title && have_subtitle && have_details;
}

const char *source_text(SaveTimeSource source) {
  switch (source) {
  case SaveTimeSource::VitaSlot: return "vita-slot";
  case SaveTimeSource::Filesystem: return "filesystem";
  case SaveTimeSource::BackupClock: return "backup-clock";
  }
  return "backup-clock";
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

std::string serialize_save_metadata_json(const std::string &identity,
                                         const SaveMetadata &metadata) {
  std::string json = "{\"version\":1,\"archiveIdentity\":";
  append_json_string(&json, identity);
  json += ",\"savedAt\":";
  append_json_string(&json, format_save_datetime(metadata.saved_at));
  json += ",\"source\":";
  append_json_string(&json, source_text(metadata.source));
  json += std::string(",\"approximate\":") + (metadata.approximate ? "true" : "false") +
          ",\"slots\":[";
  for (std::size_t i = 0; i < metadata.slots.size(); ++i) {
    const SaveSlotMetadata &slot = metadata.slots[i];
    if (i > 0) json += ',';
    json += "{\"id\":" + std::to_string(slot.id) + ",\"modifiedAt\":";
    append_json_string(&json, format_save_datetime(slot.modified_at));
    json += ",\"title\":";
    append_json_string(&json, slot.title);
    json += ",\"subtitle\":";
    append_json_string(&json, slot.subtitle);
    json += ",\"details\":";
    append_json_string(&json, slot.details);
    json += '}';
  }
  json += "]}";
  return json;
}

SaveMetadataJsonResult parse_save_metadata_json(const std::string &json) {
  SaveMetadataJsonResult result;
  if (json.size() > kMaxMetadataJsonSize) {
    result.error = "metadata is too large";
    return result;
  }
  JsonCursor cursor(json);
  if (!cursor.consume('{')) {
    result.error = "metadata object missing";
    return result;
  }
  bool have_version = false, have_identity = false, have_saved_at = false,
       have_source = false, have_approximate = false, have_slots = false;
  if (cursor.consume('}')) {
    result.error = "metadata fields missing";
    return result;
  }
  while (true) {
    std::string key;
    if (!cursor.parse_string(&key) || !cursor.consume(':')) {
      result.error = "metadata field invalid";
      return result;
    }
    if (key == "version") {
      unsigned int version = 0;
      have_version = cursor.parse_unsigned(&version) && version == 1;
      if (!have_version) {
        result.error = "unsupported metadata version";
        return result;
      }
    } else if (key == "archiveIdentity") {
      have_identity = cursor.parse_string(&result.archive_identity) &&
                      !result.archive_identity.empty() && result.archive_identity.size() <= 255;
      if (!have_identity) return result;
    } else if (key == "savedAt") {
      std::string text;
      have_saved_at = cursor.parse_string(&text) &&
                      parse_datetime_string(text, &result.metadata.saved_at);
      if (!have_saved_at) return result;
    } else if (key == "source") {
      std::string source;
      if (!cursor.parse_string(&source)) return result;
      if (source == "vita-slot") result.metadata.source = SaveTimeSource::VitaSlot;
      else if (source == "filesystem") result.metadata.source = SaveTimeSource::Filesystem;
      else if (source == "backup-clock") result.metadata.source = SaveTimeSource::BackupClock;
      else return result;
      have_source = true;
    } else if (key == "approximate") {
      have_approximate = cursor.parse_bool(&result.metadata.approximate);
      if (!have_approximate) return result;
    } else if (key == "slots") {
      if (!cursor.consume('[')) return result;
      if (!cursor.consume(']')) {
        while (true) {
          if (result.metadata.slots.size() >= kMaxSaveSlots) return result;
          SaveSlotMetadata slot;
          if (!parse_slot_json(&cursor, &slot)) return result;
          result.metadata.slots.push_back(std::move(slot));
          if (cursor.consume(']')) break;
          if (!cursor.consume(',')) return result;
        }
      }
      have_slots = true;
    } else if (!cursor.skip_value(1)) {
      return result;
    }
    if (cursor.consume('}')) break;
    if (!cursor.consume(',')) return result;
  }
  if (!cursor.at_end() || !have_version || !have_identity || !have_saved_at || !have_source ||
      !have_approximate || !have_slots) {
    result.error = "metadata fields missing";
    return result;
  }
  result.ok = true;
  return result;
}

SaveMetadataJsonResult read_save_metadata_json(const std::string &path) {
  SaveMetadataJsonResult result;
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    result.error = "metadata file missing";
    return result;
  }
  std::string json;
  std::array<char, 4096> buffer {};
  bool ok = true;
  while (true) {
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
    if (read > 0) {
      if (json.size() + read > kMaxMetadataJsonSize) {
        ok = false;
        break;
      }
      json.append(buffer.data(), read);
    }
    if (read < buffer.size()) {
      ok = std::ferror(file) == 0;
      break;
    }
  }
  std::fclose(file);
  if (!ok) {
    result.error = "metadata read failed";
    return result;
  }
  return parse_save_metadata_json(json);
}

bool write_save_metadata_json_atomic(const std::string &path,
                                     const std::string &identity,
                                     const SaveMetadata &metadata,
                                     std::string *error) {
  const std::string json = serialize_save_metadata_json(identity, metadata);
  if (json.size() > kMaxMetadataJsonSize || !is_valid_utf8(json)) {
    if (error) *error = "metadata is invalid or too large";
    return false;
  }
  const std::string temporary = path + ".tmp";
  FILE *file = std::fopen(temporary.c_str(), "wb");
  if (!file) {
    if (error) *error = "could not create metadata file";
    return false;
  }
  bool ok = std::fwrite(json.data(), 1, json.size(), file) == json.size();
  ok = std::fflush(file) == 0 && ok;
  ok = std::fclose(file) == 0 && ok;
  if (ok) {
    ok = std::rename(temporary.c_str(), path.c_str()) == 0;
  }
  if (!ok) {
    std::remove(temporary.c_str());
    if (error) *error = "could not replace metadata file";
  } else if (error) {
    error->clear();
  }
  return ok;
}

} // namespace vsm
