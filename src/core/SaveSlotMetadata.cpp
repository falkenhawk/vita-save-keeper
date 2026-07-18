#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "core/SaveSlotMetadata.hpp"

#include "core/CalendarUtil.hpp"
#include "core/PathUtil.hpp"

#include <picojson.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
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

bool valid_datetime(const SaveDateTime &value) {
  return is_valid_calendar_datetime(value.year, value.month, value.day, value.hour, value.minute,
                                    value.second);
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

// Reads a NUL-terminated fixed-capacity field. A field that fills the whole capacity with no
// terminator is treated as a maxed-out string, not corruption: dropping the slot over it would
// also discard its already-validated timestamp. The caller guarantees the field is in-bounds.
void read_fixed_string(const std::vector<unsigned char> &data, std::size_t offset,
                       std::size_t capacity, std::string *value) {
  const auto begin = data.begin() + static_cast<long>(offset);
  const auto end = begin + static_cast<long>(capacity);
  const auto terminator = std::find(begin, end, 0);
  const std::size_t length = static_cast<std::size_t>(terminator - begin);
  *value = sanitize_utf8(data.data() + offset, length);
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
  std::tm local {};
  if (!localtime_r(&value, &local)) {
    return {};
  }
  return {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,
          local.tm_sec};
}

long long days_from_civil(int year, unsigned int month, unsigned int day) {
  // Convert a Gregorian calendar date to days since the Unix epoch without consulting the
  // process timezone. sdslot.dat stores UTC calendar fields, so mktime() would mislabel them as
  // local time before the real UTC-to-local conversion happens.
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned int year_of_era = static_cast<unsigned int>(year - era * 400);
  const unsigned int shifted_month = month > 2 ? month - 3 : month + 9;
  const unsigned int day_of_year =
      (153 * shifted_month + 2) / 5 + day - 1;
  const unsigned int day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<long long>(era) * 146097 + static_cast<long long>(day_of_era) - 719468;
}

SaveDateTime local_datetime_from_utc(const SaveDateTime &utc) {
  const long long seconds =
      days_from_civil(utc.year, static_cast<unsigned int>(utc.month),
                      static_cast<unsigned int>(utc.day)) *
          86400LL +
      static_cast<long long>(utc.hour) * 3600 + utc.minute * 60 + utc.second;
  const std::time_t epoch = static_cast<std::time_t>(seconds);
  if (static_cast<long long>(epoch) != seconds) {
    return utc;
  }
  const SaveDateTime local = datetime_from_time(epoch);
  return valid_datetime(local) ? local : utc;
}

bool is_valid_utf8(const std::string &value) {
  return sanitize_utf8(reinterpret_cast<const unsigned char *>(value.data()), value.size()) == value;
}

bool json_depth_within_limit(const std::string &json) {
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char ch : json) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
    } else if (ch == '{' || ch == '[') {
      if (++depth > 32) {
        return false;
      }
    } else if (ch == '}' || ch == ']') {
      if (--depth < 0) {
        return false;
      }
    }
  }
  return depth == 0 && !in_string;
}

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

const picojson::value *json_member(const picojson::object &object, const char *key) {
  const auto found = object.find(key);
  return found == object.end() ? nullptr : &found->second;
}

bool json_string(const picojson::object &object, const char *key, std::size_t max_size,
                 std::string *out) {
  const picojson::value *value = json_member(object, key);
  if (!value || !value->is<std::string>()) {
    return false;
  }
  *out = value->get<std::string>();
  return out->size() <= max_size && is_valid_utf8(*out);
}

bool parse_slot_json(const picojson::value &value, SaveSlotMetadata *slot) {
  if (!value.is<picojson::object>()) {
    return false;
  }
  const picojson::object &object = value.get<picojson::object>();
  const picojson::value *id = json_member(object, "id");
  const picojson::value *modified = json_member(object, "modifiedAt");
  if (!id || !id->is<double>() || !modified || !modified->is<std::string>()) {
    return false;
  }
  const double number = id->get<double>();
  if (!std::isfinite(number) || number < 0 || number >= kMaxSaveSlots ||
      std::floor(number) != number) {
    return false;
  }
  slot->id = static_cast<unsigned int>(number);
  return parse_datetime_string(modified->get<std::string>(), &slot->modified_at) &&
         json_string(object, "title", kTitleSize, &slot->title) &&
         json_string(object, "subtitle", kSubtitleSize, &slot->subtitle) &&
         json_string(object, "details", kDetailsSize, &slot->details);
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
    if (!valid_datetime(slot.modified_at)) {
      continue;
    }
    read_fixed_string(data, record + kTitleOffset, kTitleSize, &slot.title);
    read_fixed_string(data, record + kSubtitleOffset, kSubtitleSize, &slot.subtitle);
    read_fixed_string(data, record + kDetailsOffset, kDetailsSize, &slot.details);
    // Load-bearing assumption: sdslot.dat stores the slot time as UTC calendar fields (observed
    // by comparing an in-game save time against the raw bytes; if it were ever local instead,
    // every displayed time would shift by the timezone offset). Normalize to device-local time
    // once here so filenames, JSON, list rows, and details all agree. See docs/save-slot-format.md.
    slot.modified_at = local_datetime_from_utc(slot.modified_at);
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

bool save_directory_has_pfs_metadata(const std::string &save_path) {
  struct stat info {};
  return stat(join_path(save_path, "sce_pfs").c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool save_metadata_is_usable(const SaveMetadataJsonResult &metadata,
                             const std::string &expected_identity) {
  return metadata.ok && metadata.archive_identity == expected_identity &&
         metadata.metadata.source != SaveTimeSource::BackupClock;
}

bool save_metadata_has_observed_time(const SaveMetadata &metadata) {
  return metadata.source == SaveTimeSource::Filesystem ||
         (metadata.source == SaveTimeSource::VitaSlot && !metadata.slots.empty());
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

bool parse_save_datetime(const std::string &text, SaveDateTime *value) {
  return parse_datetime_string(text, value);
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
  picojson::object root;
  root["version"] = picojson::value(static_cast<double>(kSaveMetadataJsonVersion));
  root["archiveIdentity"] = picojson::value(identity);
  root["savedAt"] = picojson::value(format_save_datetime(metadata.saved_at));
  root["source"] = picojson::value(source_text(metadata.source));
  root["approximate"] = picojson::value(metadata.approximate);

  picojson::array slots;
  slots.reserve(metadata.slots.size());
  for (const SaveSlotMetadata &slot : metadata.slots) {
    picojson::object item;
    item["id"] = picojson::value(static_cast<double>(slot.id));
    item["modifiedAt"] = picojson::value(format_save_datetime(slot.modified_at));
    item["title"] = picojson::value(slot.title);
    item["subtitle"] = picojson::value(slot.subtitle);
    item["details"] = picojson::value(slot.details);
    slots.emplace_back(std::move(item));
  }
  root["slots"] = picojson::value(std::move(slots));
  return picojson::value(std::move(root)).serialize();
}

SaveMetadataJsonResult parse_save_metadata_json(const std::string &json) {
  SaveMetadataJsonResult result;
  if (json.size() > kMaxMetadataJsonSize || !is_valid_utf8(json) ||
      !json_depth_within_limit(json)) {
    result.error = "metadata is invalid or too large";
    return result;
  }

  picojson::value document;
  const std::string parse_error = picojson::parse(document, json);
  if (!parse_error.empty() || !document.is<picojson::object>()) {
    result.error = parse_error.empty() ? "metadata object missing" : parse_error;
    return result;
  }
  const picojson::object &root = document.get<picojson::object>();
  const picojson::value *version = json_member(root, "version");
  const picojson::value *saved_at = json_member(root, "savedAt");
  const picojson::value *source = json_member(root, "source");
  const picojson::value *approximate = json_member(root, "approximate");
  const picojson::value *slots = json_member(root, "slots");
  const double version_number = version && version->is<double>() ? version->get<double>() : 0.0;
  const bool version_supported = std::floor(version_number) == version_number &&
                                 version_number >= kMinSaveMetadataJsonVersion &&
                                 version_number <= kSaveMetadataJsonVersion;
  if (!version || !version->is<double>() || !version_supported ||
      !json_string(root, "archiveIdentity", 255, &result.archive_identity) ||
      result.archive_identity.empty() || !saved_at || !saved_at->is<std::string>() ||
      !source || !source->is<std::string>() || !approximate || !approximate->is<bool>() ||
      !slots || !slots->is<picojson::array>()) {
    result.error = "metadata fields missing or unsupported";
    return result;
  }
  result.schema_version = static_cast<int>(version->get<double>());
  if (!parse_datetime_string(saved_at->get<std::string>(), &result.metadata.saved_at)) {
    result.error = "savedAt is invalid";
    return result;
  }

  const std::string &source_value = source->get<std::string>();
  if (source_value == "vita-slot") {
    result.metadata.source = SaveTimeSource::VitaSlot;
  } else if (source_value == "filesystem") {
    result.metadata.source = SaveTimeSource::Filesystem;
  } else if (source_value == "backup-clock") {
    result.metadata.source = SaveTimeSource::BackupClock;
  } else {
    result.error = "metadata source is unsupported";
    return result;
  }
  result.metadata.approximate = approximate->get<bool>();

  const picojson::array &slot_values = slots->get<picojson::array>();
  if (slot_values.size() > kMaxSaveSlots) {
    result.error = "too many save slots";
    return result;
  }
  result.metadata.slots.reserve(slot_values.size());
  for (const picojson::value &slot_value : slot_values) {
    SaveSlotMetadata slot;
    if (!parse_slot_json(slot_value, &slot)) {
      result.error = "save slot is invalid";
      return result;
    }
    result.metadata.slots.push_back(std::move(slot));
  }

  // Version 1 was produced before Save Keeper established that sdslot.dat calendar fields are
  // UTC. Only Vita-slot metadata needs migration; filesystem and backup-clock values were already
  // generated from the device's local clock.
  if (result.schema_version == 1 && result.metadata.source == SaveTimeSource::VitaSlot) {
    result.metadata.saved_at = local_datetime_from_utc(result.metadata.saved_at);
    for (SaveSlotMetadata &slot : result.metadata.slots) {
      slot.modified_at = local_datetime_from_utc(slot.modified_at);
    }
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
  // Write to a temp file, then rename over the target so a reader never sees a half-written
  // sidecar. No fsync: the sidecar is optional and always rebuildable from the ZIP, so durability
  // across a power cut is not worth the flush cost on the Vita's storage.
  bool wrote = std::fwrite(json.data(), 1, json.size(), file) == json.size();
  wrote = std::fflush(file) == 0 && wrote;
  wrote = std::fclose(file) == 0 && wrote;
  if (!wrote) {
    std::remove(temporary.c_str());
    if (error) *error = "could not write metadata file";
    return false;
  }
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    if (error) *error = "could not replace metadata file";
    return false;
  }
  if (error) error->clear();
  return true;
}

} // namespace vsm
