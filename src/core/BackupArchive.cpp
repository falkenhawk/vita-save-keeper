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
#include <unistd.h>
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

struct LocalZipHeader {
  std::uint16_t flags{};
  std::uint16_t method{};
  std::uint32_t crc32{};
  std::uint32_t compressed_size{};
  std::uint32_t uncompressed_size{};
  std::string name;
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

RestoreResult restore_error(std::string error) {
  RestoreResult result;
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

bool remove_tree(const std::string &path) {
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    return errno == ENOENT;
  }

  if (S_ISDIR(info.st_mode)) {
    DIR *dir = opendir(path.c_str());
    if (!dir) {
      return false;
    }

    bool ok = true;
    while (dirent *entry = readdir(dir)) {
      if (is_dot_entry(entry->d_name)) {
        continue;
      }
      ok = remove_tree(join_path(path, entry->d_name)) && ok;
    }
    closedir(dir);
    return rmdir(path.c_str()) == 0 && ok;
  }

  return std::remove(path.c_str()) == 0;
}

bool clear_directory_contents(const std::string &path) {
  if (!ensure_directory(path)) {
    return false;
  }

  DIR *dir = opendir(path.c_str());
  if (!dir) {
    return false;
  }

  bool ok = true;
  while (dirent *entry = readdir(dir)) {
    if (is_dot_entry(entry->d_name)) {
      continue;
    }
    ok = remove_tree(join_path(path, entry->d_name)) && ok;
  }
  closedir(dir);
  return ok;
}

bool move_directory_contents(const std::string &source_path, const std::string &destination_path) {
  if (!ensure_directory(destination_path)) {
    return false;
  }

  DIR *dir = opendir(source_path.c_str());
  if (!dir) {
    return false;
  }

  bool ok = true;
  while (dirent *entry = readdir(dir)) {
    if (is_dot_entry(entry->d_name)) {
      continue;
    }
    ok = std::rename(join_path(source_path, entry->d_name).c_str(),
                     join_path(destination_path, entry->d_name).c_str()) == 0 &&
         ok;
  }
  closedir(dir);
  return ok;
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

bool read_bytes(FILE *input, void *data, std::size_t size) {
  return std::fread(data, 1, size, input) == size;
}

bool read_u16(FILE *input, std::uint16_t *value) {
  unsigned char bytes[2] {};
  if (!read_bytes(input, bytes, sizeof(bytes))) {
    return false;
  }
  *value = static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
  return true;
}

bool read_u32(FILE *input, std::uint32_t *value) {
  unsigned char bytes[4] {};
  if (!read_bytes(input, bytes, sizeof(bytes))) {
    return false;
  }
  *value = static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
  return true;
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

bool skip_bytes(FILE *input, std::uint32_t size) {
  return std::fseek(input, static_cast<long>(size), SEEK_CUR) == 0;
}

bool read_string(FILE *input, std::uint16_t size, std::string *value) {
  value->assign(size, '\0');
  return size == 0 || read_bytes(input, &(*value)[0], size);
}

bool read_local_header_after_signature(FILE *zip, LocalZipHeader *header) {
  std::uint16_t version = 0;
  std::uint16_t modified_time = 0;
  std::uint16_t modified_date = 0;
  std::uint16_t name_length = 0;
  std::uint16_t extra_length = 0;
  return read_u16(zip, &version) && read_u16(zip, &header->flags) &&
         read_u16(zip, &header->method) && read_u16(zip, &modified_time) &&
         read_u16(zip, &modified_date) && read_u32(zip, &header->crc32) &&
         read_u32(zip, &header->compressed_size) && read_u32(zip, &header->uncompressed_size) &&
         read_u16(zip, &name_length) && read_u16(zip, &extra_length) &&
         read_string(zip, name_length, &header->name) && skip_bytes(zip, extra_length);
}

bool is_safe_zip_entry_path(const std::string &path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
      path.find(':') != std::string::npos) {
    return false;
  }

  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end = slash == std::string::npos ? path.size() : slash;
    const std::string part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") {
      return false;
    }
    if (slash == std::string::npos) {
      return true;
    }
    start = slash + 1;
  }

  return true;
}

bool ensure_parent_directory(const std::string &path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return true;
  }
  return ensure_directory(path.substr(0, slash));
}

bool extract_stored_file(FILE *zip, const LocalZipHeader &header,
                         const std::string &destination_path) {
  if (!ensure_parent_directory(destination_path)) {
    return false;
  }

  FILE *output = std::fopen(destination_path.c_str(), "wb");
  if (!output) {
    return false;
  }

  std::array<unsigned char, kCopyBufferSize> buffer {};
  std::uint32_t remaining = header.compressed_size;
  std::uint32_t crc = 0;
  while (remaining > 0) {
    const std::size_t chunk = std::min<std::size_t>(buffer.size(), remaining);
    if (!read_bytes(zip, buffer.data(), chunk)) {
      std::fclose(output);
      return false;
    }
    if (!write_bytes(output, buffer.data(), chunk)) {
      std::fclose(output);
      return false;
    }
    crc = update_crc32(crc, buffer.data(), chunk);
    remaining -= static_cast<std::uint32_t>(chunk);
  }

  if (std::fclose(output) != 0) {
    return false;
  }
  return crc == header.crc32;
}

bool extract_archive_to_directory(FILE *zip, const std::string &destination_path) {
  while (true) {
    std::uint32_t signature = 0;
    if (!read_u32(zip, &signature)) {
      return std::feof(zip) != 0;
    }

    if (signature == 0x02014b50 || signature == 0x06054b50) {
      return true;
    }
    if (signature != 0x04034b50) {
      return false;
    }

    LocalZipHeader header;
    if (!read_local_header_after_signature(zip, &header)) {
      return false;
    }

    // Restore only the ZIP shape Save Keeper writes today: store-method file entries with known
    // sizes in the local header. Rejecting other forms keeps restore predictable before cloud data
    // can introduce archives not produced by this app.
    if ((header.flags & 0x0008U) != 0 || header.method != 0 ||
        header.compressed_size != header.uncompressed_size ||
        !is_safe_zip_entry_path(header.name)) {
      return false;
    }

    if (!extract_stored_file(zip, header, join_path(destination_path, header.name))) {
      return false;
    }
  }
}

} // namespace

BackupResult create_backup_archive(const BackupRequest &request) {
  std::string save_folder = normalize_path_component(request.save_id);
  if (save_folder.empty()) {
    save_folder = "unknown-save";
  }

  const std::string backup_directory = join_path(request.backup_root, save_folder);
  std::string archive_name = make_timestamped_backup_name(request.timestamp);
  if (!request.name_suffix.empty()) {
    archive_name.insert(archive_name.size() - 4, request.name_suffix);
  }
  const std::string archive_path = join_path(backup_directory, archive_name);

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

std::vector<ArchiveEntryInfo> compute_folder_entries(const std::string &folder_path, bool *ok) {
  std::vector<ArchiveEntryInfo> result;
  if (ok) {
    *ok = false;
  }
  if (!is_directory(folder_path)) {
    return result;
  }

  std::vector<ZipEntry> entries;
  if (!collect_files(folder_path, "", &entries)) {
    return result;
  }
  for (ZipEntry &entry : entries) {
    if (!measure_file(&entry)) {
      return result;
    }
    result.push_back({entry.zip_path, entry.crc32, entry.size});
  }
  if (ok) {
    *ok = true;
  }
  return result;
}

bool entries_match_backup_archive(const std::vector<ArchiveEntryInfo> &folder_entries,
                                  const std::string &archive_path) {
  FILE *input = std::fopen(archive_path.c_str(), "rb");
  if (!input) {
    return false;
  }

  // Our writer emits no archive comment, so the end-of-central-directory record is exactly the
  // final 22 bytes.
  if (std::fseek(input, -22, SEEK_END) != 0) {
    std::fclose(input);
    return false;
  }
  unsigned char eocd[22];
  if (!read_bytes(input, eocd, sizeof(eocd)) ||
      !(eocd[0] == 0x50 && eocd[1] == 0x4b && eocd[2] == 0x05 && eocd[3] == 0x06)) {
    std::fclose(input);
    return false;
  }
  const std::uint16_t entry_count =
      static_cast<std::uint16_t>(eocd[10] | (eocd[11] << 8));
  const std::uint32_t central_offset =
      static_cast<std::uint32_t>(eocd[16]) | (static_cast<std::uint32_t>(eocd[17]) << 8) |
      (static_cast<std::uint32_t>(eocd[18]) << 16) |
      (static_cast<std::uint32_t>(eocd[19]) << 24);

  if (entry_count != folder_entries.size()) {
    std::fclose(input);
    return false;
  }

  std::vector<ArchiveEntryInfo> archive_entries;
  if (std::fseek(input, static_cast<long>(central_offset), SEEK_SET) != 0) {
    std::fclose(input);
    return false;
  }
  for (std::uint16_t i = 0; i < entry_count; ++i) {
    unsigned char header[46];
    if (!read_bytes(input, header, sizeof(header)) ||
        !(header[0] == 0x50 && header[1] == 0x4b && header[2] == 0x01 && header[3] == 0x02)) {
      std::fclose(input);
      return false;
    }
    const auto u32_at = [&header](int offset) {
      return static_cast<std::uint32_t>(header[offset]) |
             (static_cast<std::uint32_t>(header[offset + 1]) << 8) |
             (static_cast<std::uint32_t>(header[offset + 2]) << 16) |
             (static_cast<std::uint32_t>(header[offset + 3]) << 24);
    };
    const auto u16_at = [&header](int offset) {
      return static_cast<std::uint16_t>(header[offset] | (header[offset + 1] << 8));
    };
    const std::uint32_t crc = u32_at(16);
    const std::uint32_t uncompressed = u32_at(24);
    const std::uint16_t name_length = u16_at(28);
    const std::uint16_t extra_length = u16_at(30);
    const std::uint16_t comment_length = u16_at(32);

    std::string name(name_length, '\0');
    if (name_length > 0 && !read_bytes(input, name.data(), name_length)) {
      std::fclose(input);
      return false;
    }
    if ((extra_length > 0 || comment_length > 0) &&
        std::fseek(input, extra_length + comment_length, SEEK_CUR) != 0) {
      std::fclose(input);
      return false;
    }
    archive_entries.push_back({std::move(name), crc, uncompressed});
  }
  std::fclose(input);

  // collect_files walks children in sorted order on both sides, but sort defensively so the
  // comparison never depends on traversal details.
  std::vector<ArchiveEntryInfo> folder_sorted = folder_entries;
  const auto by_path = [](const ArchiveEntryInfo &a, const ArchiveEntryInfo &b) {
    return a.path < b.path;
  };
  std::sort(folder_sorted.begin(), folder_sorted.end(), by_path);
  std::sort(archive_entries.begin(), archive_entries.end(), by_path);
  for (std::size_t i = 0; i < folder_sorted.size(); ++i) {
    if (folder_sorted[i].path != archive_entries[i].path ||
        folder_sorted[i].crc32 != archive_entries[i].crc32 ||
        folder_sorted[i].size != archive_entries[i].size) {
      return false;
    }
  }
  return true;
}

RestoreResult restore_backup_archive(const RestoreRequest &request) {
  if (request.destination_path.empty() || request.destination_path == "/") {
    return restore_error("destination path is unsafe");
  }

  FILE *zip = std::fopen(request.archive_path.c_str(), "rb");
  if (!zip) {
    return restore_error("could not open archive");
  }

  const std::string staging_path = request.destination_path + ".restore-tmp";
  remove_tree(staging_path);
  const bool ok = ensure_directory(staging_path) && extract_archive_to_directory(zip, staging_path);
  std::fclose(zip);
  if (!ok) {
    remove_tree(staging_path);
    return restore_error("could not restore archive");
  }
  if (!clear_directory_contents(request.destination_path)) {
    remove_tree(staging_path);
    return restore_error("could not clear destination save");
  }
  if (!move_directory_contents(staging_path, request.destination_path)) {
    remove_tree(staging_path);
    return restore_error("could not replace destination save");
  }
  remove_tree(staging_path);

  RestoreResult result;
  result.ok = true;
  return result;
}

} // namespace vsm
