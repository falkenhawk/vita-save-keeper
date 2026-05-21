#include "core/BackupArchive.hpp"

#include "core/PathUtil.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace vsm {
namespace {

constexpr std::size_t kCopyBufferSize = 32 * 1024;

struct ZipTimestamp {
  std::uint16_t time{};
  std::uint16_t date{};
};

struct ZipEntry {
  std::string source_path;
  std::string zip_path;
  std::uint32_t crc32{};
  std::uint32_t size{};
  std::uint32_t local_header_offset{};
};

std::string join_path(const std::string &parent, const std::string &child) {
  if (parent.empty()) {
    return child;
  }
  if (parent.back() == '/') {
    return parent + child;
  }
  return parent + "/" + child;
}

bool is_dot_entry(const char *name) {
  return std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0;
}

bool stat_path(const std::string &path, struct stat *info) {
  return stat(path.c_str(), info) == 0;
}

bool is_directory(const std::string &path) {
  struct stat info {};
  return stat_path(path, &info) && S_ISDIR(info.st_mode);
}

bool is_regular_file(const std::string &path) {
  struct stat info {};
  return stat_path(path, &info) && S_ISREG(info.st_mode);
}

BackupResult error_result(std::string archive_path, std::string error) {
  BackupResult result;
  result.archive_path = std::move(archive_path);
  result.error = std::move(error);
  return result;
}

bool ensure_directory(const std::string &path) {
  if (path.empty()) {
    return false;
  }

  std::string current;
  std::size_t start = 0;
  if (path.front() == '/') {
    current = "/";
    start = 1;
  }

  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end = slash == std::string::npos ? path.size() : slash;
    const std::string part = path.substr(start, end - start);
    if (!part.empty()) {
      if (!current.empty() && current.back() != '/') {
        current += "/";
      }
      current += part;

      if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
        return false;
      }
      if (!is_directory(current)) {
        return false;
      }
    }

    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }

  return true;
}

bool collect_files(const std::string &directory_path, const std::string &relative_path,
                   std::vector<ZipEntry> *entries) {
  DIR *dir = opendir(directory_path.c_str());
  if (!dir) {
    return false;
  }

  std::vector<std::string> child_names;
  while (dirent *entry = readdir(dir)) {
    if (!is_dot_entry(entry->d_name)) {
      child_names.emplace_back(entry->d_name);
    }
  }
  closedir(dir);
  std::sort(child_names.begin(), child_names.end());

  for (const std::string &child_name : child_names) {
    const std::string child_source_path = join_path(directory_path, child_name);
    const std::string child_zip_path =
        relative_path.empty() ? child_name : relative_path + "/" + child_name;

    if (is_directory(child_source_path)) {
      if (!collect_files(child_source_path, child_zip_path, entries)) {
        return false;
      }
    } else if (is_regular_file(child_source_path)) {
      entries->push_back({child_source_path, child_zip_path});
    }
  }

  return true;
}

std::uint32_t update_crc32(std::uint32_t crc, const unsigned char *data, std::size_t size) {
  crc = ~crc;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = static_cast<std::uint32_t>(-(crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

bool measure_file(ZipEntry *entry) {
  FILE *input = std::fopen(entry->source_path.c_str(), "rb");
  if (!input) {
    return false;
  }

  std::uint32_t crc = 0;
  std::uint64_t size = 0;
  std::array<unsigned char, kCopyBufferSize> buffer {};
  while (true) {
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), input);
    if (read > 0) {
      crc = update_crc32(crc, buffer.data(), read);
      size += read;
    }
    if (read < buffer.size()) {
      if (std::ferror(input) != 0) {
        std::fclose(input);
        return false;
      }
      break;
    }
  }
  std::fclose(input);

  if (size > 0xffffffffULL) {
    return false;
  }
  entry->crc32 = crc;
  entry->size = static_cast<std::uint32_t>(size);
  return true;
}

bool write_bytes(FILE *output, const void *data, std::size_t size) {
  return std::fwrite(data, 1, size, output) == size;
}

bool write_string(FILE *output, const std::string &value) {
  return value.empty() || write_bytes(output, value.data(), value.size());
}

bool write_u16(FILE *output, std::uint16_t value) {
  const unsigned char bytes[] = {
      static_cast<unsigned char>(value & 0xffU),
      static_cast<unsigned char>((value >> 8U) & 0xffU),
  };
  return write_bytes(output, bytes, sizeof(bytes));
}

bool write_u32(FILE *output, std::uint32_t value) {
  const unsigned char bytes[] = {
      static_cast<unsigned char>(value & 0xffU),
      static_cast<unsigned char>((value >> 8U) & 0xffU),
      static_cast<unsigned char>((value >> 16U) & 0xffU),
      static_cast<unsigned char>((value >> 24U) & 0xffU),
  };
  return write_bytes(output, bytes, sizeof(bytes));
}

bool current_offset(FILE *file, std::uint32_t *offset) {
  const long value = std::ftell(file);
  if (value < 0 || static_cast<unsigned long>(value) > 0xffffffffUL) {
    return false;
  }
  *offset = static_cast<std::uint32_t>(value);
  return true;
}

ZipTimestamp to_zip_timestamp(const BackupTimestamp &timestamp) {
  const int year = std::max(1980, std::min(timestamp.year, 2107));
  ZipTimestamp zip_time;
  zip_time.time = static_cast<std::uint16_t>((timestamp.hour << 11U) | (timestamp.minute << 5U));
  zip_time.date =
      static_cast<std::uint16_t>(((year - 1980) << 9U) | (timestamp.month << 5U) | timestamp.day);
  return zip_time;
}

bool write_local_header(FILE *zip, ZipEntry *entry, const ZipTimestamp &timestamp) {
  if (!current_offset(zip, &entry->local_header_offset)) {
    return false;
  }

  // The writer deliberately uses ZIP "store" entries. Compression is useful later, but store-only
  // keeps this foundation dependency-free and easy to validate on both host and Vita.
  return write_u32(zip, 0x04034b50) && write_u16(zip, 20) && write_u16(zip, 0) &&
         write_u16(zip, 0) && write_u16(zip, timestamp.time) && write_u16(zip, timestamp.date) &&
         write_u32(zip, entry->crc32) && write_u32(zip, entry->size) &&
         write_u32(zip, entry->size) &&
         write_u16(zip, static_cast<std::uint16_t>(entry->zip_path.size())) && write_u16(zip, 0) &&
         write_string(zip, entry->zip_path);
}

bool write_file_data(FILE *zip, const std::string &source_path) {
  FILE *input = std::fopen(source_path.c_str(), "rb");
  if (!input) {
    return false;
  }

  std::array<unsigned char, kCopyBufferSize> buffer {};
  while (true) {
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), input);
    if (read > 0 && !write_bytes(zip, buffer.data(), read)) {
      std::fclose(input);
      return false;
    }
    if (read < buffer.size()) {
      const bool ok = std::ferror(input) == 0;
      std::fclose(input);
      return ok;
    }
  }
}

bool write_central_directory_entry(FILE *zip, const ZipEntry &entry,
                                   const ZipTimestamp &timestamp) {
  return write_u32(zip, 0x02014b50) && write_u16(zip, 20) && write_u16(zip, 20) &&
         write_u16(zip, 0) && write_u16(zip, 0) && write_u16(zip, timestamp.time) &&
         write_u16(zip, timestamp.date) && write_u32(zip, entry.crc32) &&
         write_u32(zip, entry.size) && write_u32(zip, entry.size) &&
         write_u16(zip, static_cast<std::uint16_t>(entry.zip_path.size())) && write_u16(zip, 0) &&
         write_u16(zip, 0) && write_u16(zip, 0) && write_u16(zip, 0) && write_u32(zip, 0) &&
         write_u32(zip, entry.local_header_offset) && write_string(zip, entry.zip_path);
}

bool write_end_of_central_directory(FILE *zip, std::uint16_t entry_count,
                                    std::uint32_t central_directory_size,
                                    std::uint32_t central_directory_offset) {
  return write_u32(zip, 0x06054b50) && write_u16(zip, 0) && write_u16(zip, 0) &&
         write_u16(zip, entry_count) && write_u16(zip, entry_count) &&
         write_u32(zip, central_directory_size) && write_u32(zip, central_directory_offset) &&
         write_u16(zip, 0);
}

} // namespace

BackupResult create_backup_archive(const BackupRequest &request) {
  std::string save_folder = normalize_path_component(request.save_id);
  if (save_folder.empty()) {
    save_folder = "unknown-save";
  }

  const std::string backup_directory = join_path(request.backup_root, save_folder);
  const std::string archive_path =
      join_path(backup_directory, make_timestamped_backup_name(request.timestamp));

  if (!is_directory(request.source_path)) {
    return error_result(archive_path, "source path is not a directory");
  }
  if (!ensure_directory(backup_directory)) {
    return error_result(archive_path, "could not create backup directory");
  }

  std::vector<ZipEntry> entries;
  if (!collect_files(request.source_path, "", &entries)) {
    return error_result(archive_path, "could not read source directory");
  }
  if (entries.size() > 0xffffU) {
    return error_result(archive_path, "too many files for simple ZIP archive");
  }
  for (ZipEntry &entry : entries) {
    if (entry.zip_path.size() > 0xffffU) {
      return error_result(archive_path, "file path is too long for simple ZIP archive");
    }
    if (!measure_file(&entry)) {
      return error_result(archive_path, "could not read source file");
    }
  }

  FILE *zip = std::fopen(archive_path.c_str(), "wb");
  if (!zip) {
    return error_result(archive_path, "could not create archive file");
  }

  const ZipTimestamp zip_timestamp = to_zip_timestamp(request.timestamp);
  bool ok = true;
  for (ZipEntry &entry : entries) {
    ok = write_local_header(zip, &entry, zip_timestamp) && write_file_data(zip, entry.source_path);
    if (!ok) {
      break;
    }
  }

  std::uint32_t central_directory_offset = 0;
  std::uint32_t central_directory_end = 0;
  if (ok && current_offset(zip, &central_directory_offset)) {
    for (const ZipEntry &entry : entries) {
      ok = write_central_directory_entry(zip, entry, zip_timestamp);
      if (!ok) {
        break;
      }
    }
    ok = ok && current_offset(zip, &central_directory_end);
  } else {
    ok = false;
  }

  if (ok) {
    const std::uint32_t central_directory_size =
        central_directory_end - central_directory_offset;
    ok = write_end_of_central_directory(zip, static_cast<std::uint16_t>(entries.size()),
                                        central_directory_size, central_directory_offset);
  }

  if (std::fclose(zip) != 0) {
    ok = false;
  }
  if (!ok) {
    std::remove(archive_path.c_str());
    return error_result(archive_path, "could not write archive");
  }

  BackupResult result;
  result.ok = true;
  result.archive_path = archive_path;
  return result;
}

} // namespace vsm
