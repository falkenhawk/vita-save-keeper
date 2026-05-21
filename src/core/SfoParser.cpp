#include "core/SfoParser.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace vsm {
namespace {

constexpr std::uint32_t kSfoMagic = 0x46535000;
constexpr std::size_t kHeaderSize = 20;
constexpr std::size_t kIndexSize = 16;

std::uint16_t read_le16(const std::vector<unsigned char> &data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         (static_cast<std::uint16_t>(data[offset + 1]) << 8);
}

std::uint32_t read_le32(const std::vector<unsigned char> &data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

bool has_bytes(const std::vector<unsigned char> &data, std::size_t offset, std::size_t count) {
  return offset <= data.size() && count <= data.size() - offset;
}

std::string read_c_string(const std::vector<unsigned char> &data, std::size_t offset,
                          std::size_t limit) {
  if (offset >= data.size()) {
    return {};
  }

  const std::size_t end_limit = std::min(data.size(), limit);
  std::size_t end = offset;
  while (end < end_limit && data[end] != '\0') {
    ++end;
  }
  return std::string(reinterpret_cast<const char *>(&data[offset]), end - offset);
}

bool is_string_format(std::uint16_t format) {
  return format == 0x0204 || format == 0x0004;
}

std::vector<unsigned char> read_file(const std::string &path) {
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file) {
    return {};
  }

  std::vector<unsigned char> data;
  unsigned char buffer[4096];
  while (true) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    if (read > 0) {
      data.insert(data.end(), buffer, buffer + read);
    }
    if (read < sizeof(buffer)) {
      std::fclose(file);
      return data;
    }
  }
}

} // namespace

SfoMetadata parse_sfo_metadata(const std::vector<unsigned char> &data) {
  SfoMetadata metadata;
  if (!has_bytes(data, 0, kHeaderSize)) {
    metadata.error = "SFO header is incomplete.";
    return metadata;
  }

  const std::uint32_t magic = read_le32(data, 0);
  const std::uint32_t key_table_offset = read_le32(data, 8);
  const std::uint32_t data_table_offset = read_le32(data, 12);
  const std::uint32_t entry_count = read_le32(data, 16);
  if (magic != kSfoMagic) {
    metadata.error = "SFO magic is invalid.";
    return metadata;
  }
  if (!has_bytes(data, kHeaderSize, static_cast<std::size_t>(entry_count) * kIndexSize)) {
    metadata.error = "SFO index table is incomplete.";
    return metadata;
  }
  if (key_table_offset >= data.size() || data_table_offset >= data.size() ||
      key_table_offset > data_table_offset) {
    metadata.error = "SFO table offsets are invalid.";
    return metadata;
  }

  for (std::uint32_t i = 0; i < entry_count; ++i) {
    const std::size_t index_offset = kHeaderSize + static_cast<std::size_t>(i) * kIndexSize;
    const std::uint16_t key_offset = read_le16(data, index_offset);
    const std::uint16_t param_format = read_le16(data, index_offset + 2);
    const std::uint32_t param_length = read_le32(data, index_offset + 4);
    const std::uint32_t data_offset = read_le32(data, index_offset + 12);

    if (!is_string_format(param_format)) {
      continue;
    }

    const std::size_t absolute_key_offset =
        static_cast<std::size_t>(key_table_offset) + key_offset;
    const std::size_t absolute_data_offset =
        static_cast<std::size_t>(data_table_offset) + data_offset;
    if (absolute_key_offset >= data.size() ||
        !has_bytes(data, absolute_data_offset, param_length)) {
      continue;
    }

    const std::string key = read_c_string(data, absolute_key_offset, data_table_offset);
    const std::string value =
        read_c_string(data, absolute_data_offset, absolute_data_offset + param_length);
    if (!key.empty()) {
      metadata.strings[key] = value;
    }
  }

  metadata.ok = true;
  return metadata;
}

SfoMetadata parse_sfo_metadata_file(const std::string &path) {
  const std::vector<unsigned char> data = read_file(path);
  if (data.empty()) {
    SfoMetadata metadata;
    metadata.error = "SFO file could not be read.";
    return metadata;
  }
  return parse_sfo_metadata(data);
}

std::string title_from_sfo_metadata(const SfoMetadata &metadata) {
  for (const char *key : {"TITLE", "TITLE_00", "SAVEDATA_TITLE", "SUB_TITLE"}) {
    const auto found = metadata.strings.find(key);
    if (found != metadata.strings.end() && !found->second.empty()) {
      return found->second;
    }
  }
  return {};
}

} // namespace vsm
