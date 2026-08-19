#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "core/BackupArchive.hpp"

#include "core/ContentSignature.hpp"
#include "core/DirWalk.hpp"
#include "core/PathUtil.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <utility>
#include <vector>
#include <zlib.h>

namespace vsm {
namespace {

constexpr std::size_t kCopyBufferSize = 32 * 1024;
// Byte-progress callbacks fire at most every this many bytes, bounding redraw cost while still
// animating smoothly for saves large enough to need a bar at all.
constexpr std::uint64_t kProgressReportStep = 256u * 1024u;
// Deflate's output buffer is drained inside its inner write loop every time it fills, so making it
// smaller than the input buffer is behaviorally identical - it just trades a few more loop
// iterations for 24 KB less of the main thread's stack held per open entry.
constexpr std::size_t kDeflateOutputBufferSize = 8 * 1024;
// Once this many input bytes have gone through deflate for one entry, incompressible content has
// had enough of a look: the project's known worst case is an exactly-50 MB LBP profile archive
// that never shrinks, and on a 444 MHz Vita CPU running deflate to the end just to fall back to
// store afterward burns a full extra read+deflate+rewrite pass for nothing. For a homogeneous file
// (uniformly compressible or not throughout), giving up here only costs savings it would barely
// have banked anyway - the fallback invariant only needs the deflated output to stay below the
// uncompressed size, so aborting earlier is equally safe. A mixed file (an incompressible prefix
// followed by a compressible tail) is the real tradeoff: probation judges only the part it has
// seen, so it can store a file that would have shrunk meaningfully once the compressible tail was
// reached - a measured 24.9% on one such 6 MB fixture. See the probation check in
// write_deflated_file_data.
constexpr std::uint64_t kDeflateProbationBytes = 4ull * 1024 * 1024;

struct ZipTimestamp {
  std::uint16_t time{};
  std::uint16_t date{};
};

struct ZipEntry {
  std::string source_path;
  std::string zip_path;
  ZipTimestamp modified_at;
  std::uint32_t crc32{};
  std::uint32_t size{};
  std::uint32_t compressed_size{};
  std::uint16_t method{};
  std::uint32_t local_header_offset{};
};

struct LocalZipHeader {
  std::uint16_t flags{};
  std::uint16_t method{};
  ZipTimestamp modified_at;
  std::uint32_t crc32{};
  std::uint32_t compressed_size{};
  std::uint32_t uncompressed_size{};
  std::string name;
};

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

bool is_safe_archive_name(const std::string &name) {
  return name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0 &&
         name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
         name.find(':') == std::string::npos && name != ".zip";
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
  // Entry types come from the same listing pass (d_stat on the Vita, see DirWalk) instead of a
  // stat per child - the per-path lookups made enumerating a many-thousand-file folder
  // quadratic, the same pattern that froze the issue #7 boot scan. The names are then sorted
  // and recursed in that order, so archive entry order is unchanged.
  struct Child {
    std::string name;
    bool is_directory;
    bool is_regular;
  };
  std::vector<Child> children;
  const bool opened =
      for_each_dir_entry(directory_path, [&](const DirEntryInfo &entry) {
        // an unreadable entry lists as neither type and drops out below, matching the old
        // walk's silent skip when its stat failed
        children.push_back({entry.name, entry.is_directory, entry.is_regular});
        return true;
      });
  if (!opened) {
    return false;
  }
  std::sort(children.begin(), children.end(),
            [](const Child &a, const Child &b) { return a.name < b.name; });

  for (const Child &child : children) {
    const std::string child_source_path = join_path(directory_path, child.name);
    const std::string child_zip_path =
        relative_path.empty() ? child.name : relative_path + "/" + child.name;

    if (child.is_directory) {
      if (!collect_files(child_source_path, child_zip_path, entries)) {
        return false;
      }
    } else if (child.is_regular) {
      entries->push_back({child_source_path, child_zip_path});
    }
  }

  return true;
}

std::uint32_t update_crc32_bits(std::uint32_t crc, const unsigned char *data, std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = static_cast<std::uint32_t>(-(crc & 1U));
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return crc;
}

// Chunk-chainable: callers may keep the running value between calls, starting from 0 (the ~
// complements round-trip across calls, e.g. measure_file chains this across read buffers).
std::uint32_t update_crc32(std::uint32_t crc, const unsigned char *data, std::size_t size) {
  return ~update_crc32_bits(~crc, data, size);
}

bool measure_file(ZipEntry *entry, const std::function<void(std::size_t)> &on_bytes = {},
                  const std::function<bool()> &cancel_check = {}) {
  FILE *input = std::fopen(entry->source_path.c_str(), "rb");
  if (!input) {
    return false;
  }

  std::uint32_t crc = 0;
  std::uint64_t size = 0;
  std::array<unsigned char, kCopyBufferSize> buffer {};
  while (true) {
    // Checked per chunk, not per file: a single save-slot file can be tens of megabytes, and a
    // cancel press should land within a buffer, not after the whole file is hashed.
    if (cancel_check && cancel_check()) {
      std::fclose(input);
      return false;
    }
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), input);
    if (read > 0) {
      crc = update_crc32(crc, buffer.data(), read);
      size += read;
      if (on_bytes) {
        on_bytes(read);
      }
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

std::string path_basename(const std::string &path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// A source path may be a directory (walked recursively) or a single file (one entry named
// "<prefix>/<name>"). Anything else - missing, special - is a failed collect.
bool collect_source_files(const std::string &path, const std::string &prefix,
                          std::vector<ZipEntry> *entries) {
  if (is_directory(path)) {
    return collect_files(path, prefix, entries);
  }
  if (!is_regular_file(path)) {
    return false;
  }
  const std::string name = path_basename(path);
  entries->push_back({path, prefix.empty() ? name : prefix + "/" + name});
  return true;
}

bool source_exists(const std::string &path) {
  return is_directory(path) || is_regular_file(path);
}

// Walks one directory and appends its content signature (relative path within `prefix`, CRC32,
// size) to *out. Shared by compute_folder_entries (a single directory) and compute_sources_entries
// (several directories bundled under per-source prefixes).
bool append_folder_entries(const std::string &folder_path, const std::string &prefix,
                           std::vector<ArchiveEntryInfo> *out,
                           const std::function<void(std::size_t)> &on_bytes = {},
                           const std::function<bool()> &cancel_check = {}) {
  std::vector<ZipEntry> entries;
  if (!collect_files(folder_path, prefix, &entries)) {
    return false;
  }
  for (ZipEntry &entry : entries) {
    if (!measure_file(&entry, on_bytes, cancel_check)) {
      return false;
    }
    out->push_back({entry.zip_path, entry.crc32, entry.size});
  }
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
  zip_time.time = static_cast<std::uint16_t>((timestamp.hour << 11U) |
                                             (timestamp.minute << 5U) |
                                             (timestamp.second / 2));
  zip_time.date =
      static_cast<std::uint16_t>(((year - 1980) << 9U) | (timestamp.month << 5U) | timestamp.day);
  return zip_time;
}

bool read_file_zip_timestamp(const std::string &path, ZipTimestamp *timestamp) {
  struct stat info {};
  std::tm local {};
  if (stat(path.c_str(), &info) != 0 || !localtime_r(&info.st_mtime, &local)) {
    return false;
  }
  *timestamp = to_zip_timestamp({local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                 local.tm_hour, local.tm_min, local.tm_sec});
  return true;
}

bool restore_file_zip_timestamp(const std::string &path, const ZipTimestamp &timestamp) {
  const int month = (timestamp.date >> 5U) & 0x0f;
  const int day = timestamp.date & 0x1f;
  const int hour = (timestamp.time >> 11U) & 0x1f;
  const int minute = (timestamp.time >> 5U) & 0x3f;
  const int second = (timestamp.time & 0x1f) * 2;
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  std::tm local {};
  local.tm_year = ((timestamp.date >> 9U) & 0x7f) + 80;
  local.tm_mon = month - 1;
  local.tm_mday = day;
  local.tm_hour = hour;
  local.tm_min = minute;
  local.tm_sec = second;
  local.tm_isdst = -1;
  const std::time_t modified_at = std::mktime(&local);
  if (modified_at == static_cast<std::time_t>(-1)) {
    return false;
  }
  const utimbuf times{modified_at, modified_at};
  return utime(path.c_str(), &times) == 0;
}

bool write_local_header(FILE *zip, ZipEntry *entry, const ZipTimestamp &timestamp) {
  if (!current_offset(zip, &entry->local_header_offset)) {
    return false;
  }

  // Method and compressed size are placeholders for deflate entries (patched after the data is
  // streamed); store entries carry final values immediately. The CRC pass has already run, so
  // crc32 and the uncompressed size are always exact here and no data descriptors are needed.
  return write_u32(zip, 0x04034b50) && write_u16(zip, 20) && write_u16(zip, 0) &&
         write_u16(zip, entry->method) && write_u16(zip, timestamp.time) &&
         write_u16(zip, timestamp.date) && write_u32(zip, entry->crc32) &&
         write_u32(zip, entry->compressed_size) && write_u32(zip, entry->size) &&
         write_u16(zip, static_cast<std::uint16_t>(entry->zip_path.size())) && write_u16(zip, 0) &&
         write_string(zip, entry->zip_path);
}

bool write_file_data(FILE *zip, const std::string &source_path,
                     const std::function<void(std::size_t)> &on_bytes,
                     const std::function<bool()> &cancel_check = {}) {
  FILE *input = std::fopen(source_path.c_str(), "rb");
  if (!input) {
    return false;
  }

  std::array<unsigned char, kCopyBufferSize> buffer {};
  while (true) {
    // Same per-chunk granularity as the hash pass: a cancel lands within a buffer even inside a
    // huge slot file.
    if (cancel_check && cancel_check()) {
      std::fclose(input);
      return false;
    }
    const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), input);
    if (read > 0 && !write_bytes(zip, buffer.data(), read)) {
      std::fclose(input);
      return false;
    }
    if (read > 0 && on_bytes) {
      on_bytes(read);
    }
    if (read < buffer.size()) {
      const bool ok = std::ferror(input) == 0;
      std::fclose(input);
      return ok;
    }
  }
}

enum class DeflateOutcome { Error, Deflated, StoreInstead };

// Streams one file into the zip as raw deflate. Aborts with StoreInstead the moment the output
// would reach the uncompressed size, or earlier still if the probation guard below judges the
// file incompressible from its opening bytes, so a fallback rewrite always overwrites every
// deflated byte and no stale tail can survive past the entry. Progress reports input bytes,
// mirroring the store path, so the caller's bar semantics do not change with the method.
DeflateOutcome write_deflated_file_data(FILE *zip, const std::string &source_path, int level,
                                        std::uint32_t uncompressed_size,
                                        std::uint32_t *compressed_size,
                                        const std::function<void(std::size_t)> &on_bytes,
                                        const std::function<bool()> &cancel_check) {
  FILE *input = std::fopen(source_path.c_str(), "rb");
  if (!input) {
    return DeflateOutcome::Error;
  }

  z_stream stream {};
  if (deflateInit2(&stream, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    std::fclose(input);
    return DeflateOutcome::Error;
  }

  // Filled before use on every iteration, so skipping the zero-init avoids two large memsets per
  // file; z_stream stays zeroed above because zlib requires it before deflateInit2.
  std::array<unsigned char, kCopyBufferSize> in_buffer;
  std::array<unsigned char, kDeflateOutputBufferSize> out_buffer;
  std::uint64_t total_out = 0;
  std::uint64_t consumed_input = 0;
  DeflateOutcome outcome = DeflateOutcome::Deflated;

  while (outcome == DeflateOutcome::Deflated) {
    if (cancel_check && cancel_check()) {
      outcome = DeflateOutcome::Error;
      break;
    }
    const std::size_t read = std::fread(in_buffer.data(), 1, in_buffer.size(), input);
    if (read < in_buffer.size() && std::ferror(input) != 0) {
      outcome = DeflateOutcome::Error;
      break;
    }
    const int flush = read < in_buffer.size() ? Z_FINISH : Z_NO_FLUSH;
    stream.next_in = in_buffer.data();
    stream.avail_in = static_cast<uInt>(read);

    int status = Z_OK;
    do {
      stream.next_out = out_buffer.data();
      stream.avail_out = static_cast<uInt>(out_buffer.size());
      status = deflate(&stream, flush);
      // Any status besides "made progress" or "stream finished" is unrecoverable; looping again
      // on it (Z_BUF_ERROR included) risks spinning forever, which on device means a hard hang.
      if (status != Z_OK && status != Z_STREAM_END) {
        outcome = DeflateOutcome::Error;
        break;
      }
      const std::size_t produced = out_buffer.size() - stream.avail_out;
      if (total_out + produced >= uncompressed_size) {
        outcome = DeflateOutcome::StoreInstead;
        break;
      }
      if (produced > 0 && !write_bytes(zip, out_buffer.data(), produced)) {
        outcome = DeflateOutcome::Error;
        break;
      }
      total_out += produced;
    } while (stream.avail_out == 0);

    if (outcome != DeflateOutcome::Deflated) {
      break;
    }
    consumed_input += read;
    // Probation: once enough input has gone through deflate to judge the file, less than ~1.6%
    // savings so far means the rest is unlikely to do meaningfully better either, so give up now
    // rather than after paying for the whole file (see kDeflateProbationBytes above).
    if (consumed_input >= kDeflateProbationBytes && total_out > consumed_input * 63 / 64) {
      outcome = DeflateOutcome::StoreInstead;
      break;
    }
    if (read > 0 && on_bytes) {
      on_bytes(read);
    }
    if (flush == Z_FINISH && status == Z_STREAM_END) {
      break;
    }
  }

  deflateEnd(&stream);
  std::fclose(input);
  if (outcome == DeflateOutcome::Deflated) {
    *compressed_size = static_cast<std::uint32_t>(total_out);
  }
  return outcome;
}

// Deflates a small in-memory buffer to completion. Used only for the plain-content marker, which
// is a handful of bytes, so a whole-buffer round trip is simpler than reusing the streaming writer
// above and costs nothing measurable.
bool deflate_buffer(const unsigned char *data, std::size_t size, int level,
                    std::vector<unsigned char> *out) {
  z_stream stream {};
  if (deflateInit2(&stream, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    return false;
  }
  std::array<unsigned char, 256> out_buffer {};
  stream.next_in = const_cast<unsigned char *>(data);
  stream.avail_in = static_cast<uInt>(size);
  int status = Z_OK;
  bool ok = true;
  do {
    stream.next_out = out_buffer.data();
    stream.avail_out = static_cast<uInt>(out_buffer.size());
    status = deflate(&stream, Z_FINISH);
    if (status != Z_OK && status != Z_STREAM_END) {
      ok = false;
      break;
    }
    const std::size_t produced = out_buffer.size() - stream.avail_out;
    out->insert(out->end(), out_buffer.data(), out_buffer.data() + produced);
  } while (status != Z_STREAM_END);
  deflateEnd(&stream);
  return ok;
}

// Verbatim content of the kPlainFormatEntryName entry (Amendment B). ~165 bytes of ASCII prose -
// long enough that deflate always wins (a test pins method 8 + compressed < uncompressed) - naming
// the format version on its first line (parse_plain_format_version reads the trailing integer
// there) and documenting the archive for a PC user who opens it directly.
const std::string kFormatEntryContent =
    "save-keeper compatibility marker - format 1\n"
    "\n"
    "game files in this archive are stored decrypted; .raw/ holds the\n"
    "console's encrypted sce_sys + sce_pfs snapshot used at restore.\n";

// Writes kPlainFormatEntryName (".savekeeper") as a complete, self-contained entry: its content and
// size are fixed and tiny, so - unlike the streaming file writer above - both the compressed size
// and the CRC are known before the local header goes out, and no placeholder patch is needed
// afterward. Always method 8: the content reliably deflates on any zlib level, and admitting
// method 0 here would let an app version that predates deflate support (which rejects any
// method-8 entry before touching a live save) read mounted decrypted bytes as if they were the
// archive's raw on-disk shape (see BackupRequest::add_plain_format_entry). level is clamped to at
// least 1 defensively - the entry is only ever requested alongside compression_level >= 1, but a
// caller that got that wrong must still never produce a store entry here.
bool write_plain_format_entry(FILE *zip, int level, const ZipTimestamp &timestamp,
                              ZipEntry *out_entry) {
  ZipEntry entry;
  entry.zip_path = kPlainFormatEntryName;
  entry.modified_at = timestamp;
  entry.crc32 = update_crc32(0, reinterpret_cast<const unsigned char *>(kFormatEntryContent.data()),
                             kFormatEntryContent.size());
  entry.size = static_cast<std::uint32_t>(kFormatEntryContent.size());
  entry.method = 8;

  std::vector<unsigned char> compressed;
  if (!deflate_buffer(reinterpret_cast<const unsigned char *>(kFormatEntryContent.data()),
                      kFormatEntryContent.size(), std::max(1, level), &compressed)) {
    return false;
  }
  entry.compressed_size = static_cast<std::uint32_t>(compressed.size());

  if (!write_local_header(zip, &entry, timestamp) ||
      !write_bytes(zip, compressed.data(), compressed.size())) {
    return false;
  }
  *out_entry = entry;
  return true;
}

// Writes one small, wholly in-memory entry (the .raw/ skeleton files - see
// BackupRequest::raw_entries): same deflate-with-store-fallback rule as a file entry
// (write_entry_data), but with no seek-back patch needed, since the whole compressed buffer (if
// any) is produced before any bytes reach the zip, so the final method and compressed size are
// already known when the local header goes out. Encrypted PFS content does not compress in
// practice, so these entries are expected to store; level 0 or an empty buffer stores
// unconditionally, exactly like the main writer's whole-archive level-0 rule.
bool write_in_memory_entry(FILE *zip, const std::string &zip_path, const unsigned char *data,
                           std::size_t size, int level, const ZipTimestamp &timestamp,
                           ZipEntry *out_entry) {
  if (size > 0xffffffffULL) {
    return false;
  }
  ZipEntry entry;
  entry.zip_path = zip_path;
  entry.modified_at = timestamp;
  entry.crc32 = update_crc32(0, data, size);
  entry.size = static_cast<std::uint32_t>(size);

  std::vector<unsigned char> compressed;
  const bool deflated =
      level > 0 && size > 0 && deflate_buffer(data, size, level, &compressed) &&
      compressed.size() < size;
  if (deflated) {
    entry.method = 8;
    entry.compressed_size = static_cast<std::uint32_t>(compressed.size());
    if (!write_local_header(zip, &entry, timestamp) ||
        !write_bytes(zip, compressed.data(), compressed.size())) {
      return false;
    }
  } else {
    entry.method = 0;
    entry.compressed_size = entry.size;
    if (!write_local_header(zip, &entry, timestamp) ||
        (size > 0 && !write_bytes(zip, data, size))) {
      return false;
    }
  }
  *out_entry = entry;
  return true;
}

// Writes one entry's data: a placeholder local header (entry->method and entry->compressed_size
// must already hold the entry's chosen method and its placeholder size, e.g. entry->size), then
// either a stored copy or a deflate attempt that falls back to store when it does not shrink the
// file, then a seek-back patch of the header's method and compressed-size fields once both are
// final. entry->method and entry->compressed_size are updated in place to their final values -
// the store fallback flips method back to 0 - so callers never juggle a separate out-param.
bool write_entry_data(FILE *zip, ZipEntry *entry, int level,
                      const std::function<void(std::size_t)> &on_bytes,
                      const std::function<bool()> &cancel_check) {
  if (!write_local_header(zip, entry, entry->modified_at)) {
    return false;
  }
  if (entry->method == 0) {
    return write_file_data(zip, entry->source_path, on_bytes, cancel_check);
  }

  std::uint64_t reported = 0;
  const std::function<void(std::size_t)> on_deflate_bytes =
      on_bytes ? std::function<void(std::size_t)>([&](std::size_t chunk) {
        reported += chunk;
        on_bytes(chunk);
      })
               : std::function<void(std::size_t)>();
  std::uint32_t data_start = 0;
  if (!current_offset(zip, &data_start)) {
    return false;
  }
  std::uint32_t compressed_size = 0;
  const DeflateOutcome outcome = write_deflated_file_data(
      zip, entry->source_path, level, entry->size, &compressed_size, on_deflate_bytes, cancel_check);
  if (outcome == DeflateOutcome::Error) {
    return false;
  }
  if (outcome == DeflateOutcome::StoreInstead) {
    // Deflate could not beat store for this file (already-compressed content, or the probation
    // guard gave up early). Rewind and store it; every deflated byte sits below entry->size, so
    // the raw copy overwrites all of them and the entry ends exactly at data_start + entry->size.
    entry->method = 0;
    compressed_size = entry->size;
    std::uint64_t skip = reported;
    const std::function<void(std::size_t)> on_remaining_bytes =
        on_bytes ? std::function<void(std::size_t)>([&](std::size_t chunk) {
          if (skip >= chunk) {
            skip -= chunk;
            return;
          }
          on_bytes(chunk - static_cast<std::size_t>(skip));
          skip = 0;
        })
                 : std::function<void(std::size_t)>();
    if (std::fseek(zip, static_cast<long>(data_start), SEEK_SET) != 0 ||
        !write_file_data(zip, entry->source_path, on_remaining_bytes, cancel_check)) {
      return false;
    }
  }
  entry->compressed_size = compressed_size;

  // Patch the placeholder method and compressed size now that both are known, then return to the
  // entry's end for the next header. CRC and uncompressed size were exact up front. In the local
  // header layout, signature (4) + version (2) + flags (2) puts method at +8; crc32 follows method
  // and mod time/date at +14; compressed size follows crc32 at +18.
  std::uint32_t data_end = 0;
  return current_offset(zip, &data_end) &&
         std::fseek(zip, static_cast<long>(entry->local_header_offset) + 8, SEEK_SET) == 0 &&
         write_u16(zip, entry->method) &&
         std::fseek(zip, static_cast<long>(entry->local_header_offset) + 18, SEEK_SET) == 0 &&
         write_u32(zip, entry->compressed_size) &&
         std::fseek(zip, static_cast<long>(data_end), SEEK_SET) == 0;
}

bool write_central_directory_entry(FILE *zip, const ZipEntry &entry,
                                   const ZipTimestamp &timestamp) {
  return write_u32(zip, 0x02014b50) && write_u16(zip, 20) && write_u16(zip, 20) &&
         write_u16(zip, 0) && write_u16(zip, entry.method) && write_u16(zip, timestamp.time) &&
         write_u16(zip, timestamp.date) && write_u32(zip, entry.crc32) &&
         write_u32(zip, entry.compressed_size) && write_u32(zip, entry.size) &&
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
  std::uint16_t name_length = 0;
  std::uint16_t extra_length = 0;
  return read_u16(zip, &version) && read_u16(zip, &header->flags) &&
         read_u16(zip, &header->method) && read_u16(zip, &header->modified_at.time) &&
         read_u16(zip, &header->modified_at.date) && read_u32(zip, &header->crc32) &&
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

// Streams one entry's deflate data from zip into output, inflating it, updating *crc as it goes
// (chunk-chained, so the caller starts *crc at 0 and compares the final value itself), and calling
// on_chunk once per produced chunk for progress reporting. Guards both ends of the stream: a
// zip-bomb ceiling on total output (the header's uncompressed size), and an exact-match requirement
// at the end (no leftover compressed bytes, no short output) so a truncated or corrupt deflate
// stream is reported as failure rather than silently producing partial data.
bool inflate_entry_to_file(FILE *zip, const LocalZipHeader &header, FILE *output,
                           std::uint32_t *crc,
                           const std::function<void(std::size_t)> &on_chunk) {
  z_stream stream {};
  if (inflateInit2(&stream, -15) != Z_OK) {
    return false;
  }
  std::array<unsigned char, kCopyBufferSize> in_buffer {};
  std::array<unsigned char, kCopyBufferSize> out_buffer {};
  std::uint32_t remaining = header.compressed_size;
  std::uint64_t total_out = 0;
  bool ok = true;
  int status = Z_OK;
  while (status != Z_STREAM_END) {
    if (stream.avail_in == 0) {
      if (remaining == 0) {
        // Compressed bytes ran out before the stream ended: truncated or corrupt entry.
        ok = false;
        break;
      }
      const std::size_t chunk = std::min<std::size_t>(in_buffer.size(), remaining);
      if (!read_bytes(zip, in_buffer.data(), chunk)) {
        ok = false;
        break;
      }
      remaining -= static_cast<std::uint32_t>(chunk);
      stream.next_in = in_buffer.data();
      stream.avail_in = static_cast<uInt>(chunk);
    }
    stream.next_out = out_buffer.data();
    stream.avail_out = static_cast<uInt>(out_buffer.size());
    status = inflate(&stream, Z_NO_FLUSH);
    if (status != Z_OK && status != Z_STREAM_END) {
      ok = false;
      break;
    }
    const std::size_t produced = out_buffer.size() - stream.avail_out;
    // Zip-bomb guard: the header's uncompressed size is the hard ceiling.
    if (total_out + produced > header.uncompressed_size) {
      ok = false;
      break;
    }
    if (produced > 0) {
      if (!write_bytes(output, out_buffer.data(), produced)) {
        ok = false;
        break;
      }
      *crc = update_crc32(*crc, out_buffer.data(), produced);
      total_out += produced;
      if (on_chunk) {
        on_chunk(produced);
      }
    }
  }
  // The stream must end exactly at the recorded sizes: no leftover compressed bytes, no short
  // output.
  ok = ok && status == Z_STREAM_END && remaining == 0 && stream.avail_in == 0 &&
       total_out == header.uncompressed_size;
  inflateEnd(&stream);
  return ok;
}

// In-memory counterpart of inflate_entry_to_file for the bounded single-entry reader: *out must
// already be sized to header.uncompressed_size (enforced below rather than assumed), and inflate
// writes directly into it - no scratch buffer or running-offset copy needed, since zlib can never
// write past avail_out, so pinning avail_out to out->size() up front makes overproduction
// structurally impossible; no separate zip-bomb ceiling check is needed either.
bool inflate_entry_to_buffer(FILE *zip, const LocalZipHeader &header,
                             std::vector<unsigned char> *out) {
  if (out->size() != header.uncompressed_size) {
    return false;
  }
  z_stream stream {};
  if (inflateInit2(&stream, -15) != Z_OK) {
    return false;
  }
  std::array<unsigned char, kCopyBufferSize> in_buffer {};
  std::uint32_t remaining = header.compressed_size;
  stream.next_out = out->data();
  stream.avail_out = static_cast<uInt>(out->size());
  bool ok = true;
  int status = Z_OK;
  while (status != Z_STREAM_END) {
    if (stream.avail_in == 0) {
      if (remaining == 0) {
        // Compressed bytes ran out before the stream ended: truncated or corrupt entry.
        ok = false;
        break;
      }
      const std::size_t chunk = std::min<std::size_t>(in_buffer.size(), remaining);
      if (!read_bytes(zip, in_buffer.data(), chunk)) {
        ok = false;
        break;
      }
      remaining -= static_cast<std::uint32_t>(chunk);
      stream.next_in = in_buffer.data();
      stream.avail_in = static_cast<uInt>(chunk);
    }
    status = inflate(&stream, Z_NO_FLUSH);
    if (status != Z_OK && status != Z_STREAM_END) {
      ok = false;
      break;
    }
  }
  // The stream must end exactly at the recorded sizes: no leftover compressed bytes, no short
  // output - avail_out == 0 is the short-output guard.
  ok = ok && status == Z_STREAM_END && remaining == 0 && stream.avail_in == 0 &&
       stream.avail_out == 0;
  inflateEnd(&stream);
  return ok;
}

// Extracts one entry (store or deflate) to destination_path, CRC-checking the uncompressed bytes
// against the header regardless of method.
bool extract_file(FILE *zip, const LocalZipHeader &header, const std::string &destination_path,
                  const std::function<void(std::size_t)> &on_chunk = {}) {
  if (!ensure_parent_directory(destination_path)) {
    return false;
  }

  FILE *output = std::fopen(destination_path.c_str(), "wb");
  if (!output) {
    return false;
  }

  std::uint32_t crc = 0;
  bool ok = true;
  if (header.method == 0) {
    std::array<unsigned char, kCopyBufferSize> buffer {};
    std::uint32_t remaining = header.compressed_size;
    while (remaining > 0) {
      const std::size_t chunk = std::min<std::size_t>(buffer.size(), remaining);
      if (!read_bytes(zip, buffer.data(), chunk) || !write_bytes(output, buffer.data(), chunk)) {
        ok = false;
        break;
      }
      crc = update_crc32(crc, buffer.data(), chunk);
      remaining -= static_cast<std::uint32_t>(chunk);
      if (on_chunk) {
        on_chunk(chunk);
      }
    }
  } else {
    ok = inflate_entry_to_file(zip, header, output, &crc, on_chunk);
  }

  if (std::fclose(output) != 0) {
    return false;
  }
  if (!ok || crc != header.crc32) {
    return false;
  }
  // Restoring the original modification time is best-effort. A flaky utime() on the target
  // filesystem or an out-of-range DOS timestamp in an imported ZIP must never turn an otherwise
  // complete, CRC-verified restore into a failure that leaves the user without their save.
  restore_file_zip_timestamp(destination_path, header.modified_at);
  return true;
}

bool extract_archive_to_directory(
    FILE *zip, const std::string &destination_path, std::uint64_t max_total_bytes,
    bool *file_timestamps_uniform = nullptr,
    const std::function<void(std::uint64_t, std::uint64_t)> &progress = {},
    std::uint64_t *extracted_content_bytes = nullptr, std::uint64_t output_total = 0) {
  // Two progress modes. With output_total (the caller pre-summed the entries' uncompressed
  // sizes): report bytes PRODUCED against it - inflate's effort is proportional to output, so
  // this is the bar that tracks real work; a highly compressed entry no longer crawls the bar
  // while the console works hardest. Without it: legacy mode, bytes consumed from the archive
  // stream via the stream position, which is what the raw restore bar has always shown.
  std::uint64_t archive_bytes = 0;
  std::uint64_t last_reported = 0;
  std::uint64_t produced_total = 0;
  std::function<void(std::size_t)> on_chunk;
  if (progress) {
    const long start = std::ftell(zip);
    if (start >= 0 && std::fseek(zip, 0, SEEK_END) == 0) {
      const long end = std::ftell(zip);
      if (end > 0 && std::fseek(zip, start, SEEK_SET) == 0) {
        archive_bytes = static_cast<std::uint64_t>(end);
      }
    }
    const std::uint64_t report_total = output_total > 0 ? output_total : archive_bytes;
    progress(0, report_total);
    // Report roughly 128 times across the run regardless of size: a fixed 256 KB step never
    // fires for an archive smaller than that (a slim plain archive is ~100 KB), which showed up
    // on hardware as a restore bar frozen at zero through the whole extract phase - and 32 steps
    // still felt like one sluggish jump per second on a multi-minute extract. Large runs still
    // step at most every kProgressReportStep, bounding redraw cost.
    const std::uint64_t report_step = std::min<std::uint64_t>(
        kProgressReportStep, std::max<std::uint64_t>(report_total / 128, 4u * 1024u));
    on_chunk = [&last_reported, &produced_total, &progress, archive_bytes, output_total,
                report_step, zip](std::size_t produced) {
      std::uint64_t done = 0;
      std::uint64_t total = 0;
      if (output_total > 0) {
        produced_total += produced;
        done = produced_total;
        total = output_total;
      } else {
        const long position = std::ftell(zip);
        if (position < 0) {
          return;
        }
        done = static_cast<std::uint64_t>(position);
        total = archive_bytes;
      }
      if (done - last_reported >= report_step) {
        last_reported = done;
        progress(std::min(done, total), total);
      }
    };
  }

  std::uint64_t total_bytes = 0;
  std::size_t entry_count = 0;
  ZipTimestamp first_timestamp;
  bool timestamps_uniform = true;
  while (true) {
    std::uint32_t signature = 0;
    if (!read_u32(zip, &signature)) {
      // A valid ZIP must reach its central-directory or end record. EOF after local entries is a
      // truncated download, not a successfully extracted archive.
      return false;
    }

    if (signature == 0x02014b50 || signature == 0x06054b50) {
      if (file_timestamps_uniform) {
        // A single entry counts as uniform on purpose: one file's timestamp is indistinguishable
        // from a legacy synthetic backup stamp, so its filesystem time is not trusted as a save
        // time. The caller then leaves the time unknown rather than risk showing the backup time.
        *file_timestamps_uniform = entry_count > 0 && timestamps_uniform;
      }
      if (progress) {
        const std::uint64_t report_total = output_total > 0 ? output_total : archive_bytes;
        progress(report_total, report_total);
      }
      if (extracted_content_bytes) {
        *extracted_content_bytes = total_bytes;
      }
      return true;
    }
    if (signature != 0x04034b50) {
      return false;
    }

    LocalZipHeader header;
    if (!read_local_header_after_signature(zip, &header)) {
      return false;
    }

    // Restore only the ZIP shapes Save Keeper writes today: store and deflate file entries with
    // known sizes in the local header. Rejecting other forms keeps restore predictable before
    // cloud data can introduce archives not produced by this app.
    const bool store_shape =
        header.method == 0 && header.compressed_size == header.uncompressed_size;
    const bool deflate_shape =
        header.method == 8 && header.compressed_size < header.uncompressed_size;
    if ((header.flags & 0x0008U) != 0 || (!store_shape && !deflate_shape) ||
        !is_safe_zip_entry_path(header.name)) {
      return false;
    }

    // The plain-content marker/format entries are reserved control entries, never real savedata:
    // read past their data without creating a file, and before either can count against the
    // entry/byte caps or the timestamp-uniformity check below, so neither can ever materialize in
    // a restored save or an inspection directory. .raw/ skeleton entries are NOT skipped here -
    // the two-phase restore's own work-directory extraction needs them on disk (App.cpp reads them
    // back out explicitly, by their known .raw/sce_sys and .raw/sce_pfs paths, right after this
    // call returns). Raw archives never contain either control name, so their behavior is
    // unchanged.
    if (header.name == kPlainContentMarkerName || header.name == kPlainFormatEntryName) {
      if (!skip_bytes(zip, header.compressed_size)) {
        return false;
      }
      continue;
    }

    // Inspection accepts cloud-provided ZIPs, so cap both archive expansion and header churn
    // before creating the next file. Save Keeper's own writer is already limited to ZIP32.
    if (++entry_count > 0xffffU || header.uncompressed_size > max_total_bytes - total_bytes) {
      return false;
    }
    total_bytes += header.uncompressed_size;

    if (entry_count == 1) {
      first_timestamp = header.modified_at;
    } else if (header.modified_at.time != first_timestamp.time ||
               header.modified_at.date != first_timestamp.date) {
      timestamps_uniform = false;
    }

    if (!extract_file(zip, header, join_path(destination_path, header.name), on_chunk)) {
      return false;
    }
  }
}

// Recursively sums the byte size of every regular file under a directory. Returns false when the
// directory (or a subdirectory) could not be opened, but still adds whatever it did read.
// No entry cap here: the details view must report the folder's true size however large it is,
// and with d_stat-backed listing (DirWalk) even a many-thousand-file folder walks linearly.
bool add_directory_size(const std::string &path, std::uint64_t *total, std::size_t *files) {
  bool ok = true;
  const bool opened = for_each_dir_entry(path, [&](const DirEntryInfo &entry) {
    if (!entry.stat_ok) {
      ok = false;
      return true;
    }
    if (entry.is_directory) {
      if (!add_directory_size(join_path(path, entry.name), total, files)) {
        ok = false;
      }
    } else if (entry.is_regular) {
      *total += static_cast<std::uint64_t>(entry.size);
      if (files) {
        ++*files;
      }
    }
    return true;
  });
  return opened && ok;
}

} // namespace

BackupResult create_backup_archive(const BackupRequest &request) {
  std::string save_folder = normalize_path_component(request.save_id);
  if (save_folder.empty()) {
    save_folder = "unknown-save";
  }

  const std::string backup_directory = join_path(request.backup_root, save_folder);
  if (!request.archive_name.empty() && !is_safe_archive_name(request.archive_name)) {
    return error_result(join_path(backup_directory, request.archive_name), "archive name is unsafe");
  }
  std::string archive_name = request.archive_name.empty()
                                 ? make_timestamped_backup_name(request.timestamp)
                                 : request.archive_name;
  if (request.archive_name.empty() && !request.name_suffix.empty()) {
    archive_name.insert(archive_name.size() - 4, request.name_suffix);
  }
  const std::string archive_path = join_path(backup_directory, archive_name);

  const bool multi_source = !request.sources.empty();
  if (multi_source) {
    if (!tracked_paths_are_well_formed(request.sources)) {
      return error_result(archive_path, "tracked paths are misconfigured");
    }
    bool any_source_exists = false;
    for (const BackupSource &source : request.sources) {
      if (source_exists(source.path)) {
        any_source_exists = true;
        break;
      }
    }
    if (!any_source_exists) {
      return error_result(archive_path, "no source path exists");
    }
  } else if (!is_directory(request.source_path)) {
    return error_result(archive_path, "source path is not a directory");
  }
  if (!ensure_directory(backup_directory)) {
    return error_result(archive_path, "could not create backup directory");
  }

  std::vector<ZipEntry> entries;
  if (multi_source) {
    for (const BackupSource &source : request.sources) {
      // A tracked path missing on disk (a core the user never ran, say) simply contributes
      // nothing to the archive; only a wholly absent set of sources is an error, checked above.
      if (source_exists(source.path) &&
          !collect_source_files(source.path, source.prefix, &entries)) {
        return error_result(archive_path, "could not read source directory");
      }
    }
  } else if (!collect_files(request.source_path, "", &entries)) {
    return error_result(archive_path, "could not read source directory");
  }
  if (entries.size() + (request.add_plain_format_entry ? 1u : 0u) + request.raw_entries.size() >
      0xffffU) {
    return error_result(archive_path, "too many files for simple ZIP archive");
  }
  for (const InMemoryEntry &raw_entry : request.raw_entries) {
    if (raw_entry.zip_path.size() > 0xffffU) {
      return error_result(archive_path, "file path is too long for simple ZIP archive");
    }
  }

  // The archive reads every byte twice - a hash pass for the entry headers, then the write pass -
  // but reports one continuous bar: the hash pass fills the first half and the write continues
  // from the midpoint. The reported denominator is the real content size, not the internal
  // two-pass budget - the modal prints these numbers as "X MB of Y MB" now, and a doubled total
  // read as a save twice its size - so each pass advances the shown bytes at half rate. Stat
  // sizes fix the denominator up front; they are advisory, so a file changing mid-scan just
  // clamps in range.
  std::uint64_t stat_total = 0;
  std::uint64_t hashed = 0;
  std::uint64_t hash_reported = 0;
  std::function<void(std::size_t)> on_hash_bytes;
  if (request.progress) {
    for (const ZipEntry &entry : entries) {
      struct stat info {};
      if (stat(entry.source_path.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
        stat_total += static_cast<std::uint64_t>(info.st_size);
      }
    }
    request.progress(0, stat_total);
    on_hash_bytes = [&](std::size_t chunk) {
      hashed += chunk;
      if (hashed - hash_reported >= kProgressReportStep) {
        hash_reported = hashed;
        request.progress(std::min(hashed, stat_total) / 2, stat_total);
      }
    };
  }

  for (ZipEntry &entry : entries) {
    if (entry.zip_path.size() > 0xffffU) {
      return error_result(archive_path, "file path is too long for simple ZIP archive");
    }
    if (!measure_file(&entry, on_hash_bytes, request.cancel_check) ||
        !read_file_zip_timestamp(entry.source_path, &entry.modified_at)) {
      return error_result(archive_path, "could not read source file");
    }
  }

  const int descriptor = open(archive_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
  if (descriptor < 0) {
    return error_result(archive_path, errno == EEXIST ? "archive already exists"
                                                      : "could not create archive file");
  }
  FILE *zip = fdopen(descriptor, "wb");
  if (!zip) {
    close(descriptor);
    std::remove(archive_path.c_str());
    return error_result(archive_path, "could not create archive stream");
  }

  // Sizes are known now (measure_file above), so the write can report determinate byte progress
  // for a caller that wants to animate a bar. Throttle to ~every 256 KB to bound redraw cost.
  std::uint64_t total_bytes = 0;
  for (const ZipEntry &entry : entries) {
    total_bytes += entry.size;
  }
  std::uint64_t written = 0;
  std::uint64_t last_reported = 0;
  const std::function<void(std::size_t)> on_bytes =
      request.progress ? std::function<void(std::size_t)>([&](std::size_t chunk) {
        written += chunk;
        if (written - last_reported >= kProgressReportStep || written == total_bytes) {
          last_reported = written;
          // The write pass continues the same bar from the hash pass's midpoint.
          request.progress(std::min(stat_total + written, 2 * stat_total) / 2, stat_total);
        }
      })
                       : std::function<void(std::size_t)>();

  const int level = std::max(0, std::min(request.compression_level, 9));

  // The format entry and raw skeleton entries (when requested) are written first and outside the
  // entries vector: they have no source_path, so they never go through write_entry_data's
  // file-based streaming, and their final sizes are known up front (no placeholder patch needed).
  // They still participate in the central directory and entry count like any entry, in this fixed
  // order: the format entry, then each raw skeleton entry, then the game files. Neither one feeds
  // stat_total or on_bytes above (they are outside the `entries` vector those are built from), so
  // they are deliberately excluded from the progress bar's denominator and byte count - the bar
  // tracks real save content, and a skeleton is typically under a megabyte against saves the bar
  // is meant for.
  ZipEntry format_entry;
  std::vector<ZipEntry> raw_zip_entries;
  raw_zip_entries.reserve(request.raw_entries.size());
  bool ok = true;
  if (request.add_plain_format_entry) {
    ok = write_plain_format_entry(zip, level, to_zip_timestamp(request.timestamp), &format_entry);
  }
  if (ok) {
    for (const InMemoryEntry &raw_entry : request.raw_entries) {
      ZipEntry written_raw_entry;
      if (!write_in_memory_entry(zip, raw_entry.zip_path, raw_entry.data.data(),
                                 raw_entry.data.size(), level, to_zip_timestamp(request.timestamp),
                                 &written_raw_entry)) {
        ok = false;
        break;
      }
      raw_zip_entries.push_back(written_raw_entry);
    }
  }

  for (ZipEntry &entry : entries) {
    if (!ok) {
      break;
    }
    entry.method = (level > 0 && entry.size > 0) ? 8 : 0;
    entry.compressed_size = entry.size;
    if (!write_entry_data(zip, &entry, level, on_bytes, request.cancel_check)) {
      ok = false;
      break;
    }
  }

  std::uint32_t central_directory_offset = 0;
  std::uint32_t central_directory_end = 0;
  if (ok && current_offset(zip, &central_directory_offset)) {
    if (request.add_plain_format_entry) {
      ok = write_central_directory_entry(zip, format_entry, format_entry.modified_at);
    }
    for (const ZipEntry &raw_entry : raw_zip_entries) {
      if (!ok) {
        break;
      }
      ok = write_central_directory_entry(zip, raw_entry, raw_entry.modified_at);
    }
    for (const ZipEntry &entry : entries) {
      if (!ok) {
        break;
      }
      ok = write_central_directory_entry(zip, entry, entry.modified_at);
    }
    ok = ok && current_offset(zip, &central_directory_end);
  } else {
    ok = false;
  }

  if (ok) {
    const std::uint32_t central_directory_size =
        central_directory_end - central_directory_offset;
    const std::uint16_t total_entry_count = static_cast<std::uint16_t>(
        entries.size() + (request.add_plain_format_entry ? 1u : 0u) + raw_zip_entries.size());
    ok = write_end_of_central_directory(zip, total_entry_count, central_directory_size,
                                        central_directory_offset);
  }

  if (std::fclose(zip) != 0) {
    ok = false;
  }
  if (!ok) {
    std::remove(archive_path.c_str());
    return error_result(archive_path, "could not write archive");
  }
  if (request.progress) {
    // Land exactly on full regardless of advisory stat drift.
    request.progress(stat_total, stat_total);
  }

  BackupResult result;
  result.ok = true;
  result.archive_path = archive_path;
  return result;
}

std::vector<ArchiveEntryInfo> compute_folder_entries(
    const std::string &folder_path, bool *ok,
    const std::function<void(std::uint64_t, std::uint64_t)> &progress,
    const std::function<bool()> &cancel_check) {
  std::vector<ArchiveEntryInfo> result;
  if (ok) {
    *ok = false;
  }
  if (!is_directory(folder_path)) {
    return result;
  }

  // A cheap size pass fixes the denominator before any hashing starts; the sizes are advisory
  // (a file growing mid-scan just clamps at 100%), which is fine for a progress bar.
  std::uint64_t total_bytes = 0;
  std::uint64_t hashed = 0;
  std::uint64_t last_reported = 0;
  std::function<void(std::size_t)> on_bytes;
  if (progress) {
    add_directory_size(folder_path, &total_bytes, nullptr);
    progress(0, total_bytes);
    on_bytes = [&](std::size_t chunk) {
      hashed += chunk;
      if (hashed - last_reported >= kProgressReportStep) {
        last_reported = hashed;
        progress(std::min(hashed, total_bytes), total_bytes);
      }
    };
  }

  const bool walked = append_folder_entries(folder_path, "", &result, on_bytes, cancel_check);
  if (progress) {
    progress(total_bytes, total_bytes);
  }
  if (ok) {
    *ok = walked;
  }
  return result;
}

std::vector<ArchiveEntryInfo> compute_sources_entries(const std::vector<BackupSource> &sources,
                                                      bool *ok,
                                                      const std::function<bool()> &cancel_check) {
  std::vector<ArchiveEntryInfo> result;
  bool walked_cleanly = true;
  for (const BackupSource &source : sources) {
    // A source missing on disk contributes nothing; it is not itself a failure to walk.
    if (!source_exists(source.path)) {
      continue;
    }
    std::vector<ZipEntry> entries;
    if (!collect_source_files(source.path, source.prefix, &entries)) {
      walked_cleanly = false;
      break;
    }
    for (ZipEntry &entry : entries) {
      if (!measure_file(&entry, {}, cancel_check)) {
        walked_cleanly = false;
        break;
      }
      result.push_back({entry.zip_path, entry.crc32, entry.size});
    }
    if (!walked_cleanly) {
      break;
    }
  }
  if (ok) {
    *ok = walked_cleanly;
  }
  return result;
}

// Reads the entry list (relative path, CRC32, uncompressed size) from a Save Keeper archive's
// central directory without decompressing. Our writer emits no archive comment, so the
// end-of-central-directory record is exactly the final 22 bytes. Returns false for anything that
// is not one of our readable ZIPs.
bool read_archive_central_directory(const std::string &archive_path,
                                    std::vector<ArchiveEntryInfo> *out) {
  FILE *input = std::fopen(archive_path.c_str(), "rb");
  if (!input) {
    return false;
  }
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
  const std::uint16_t entry_count = static_cast<std::uint16_t>(eocd[10] | (eocd[11] << 8));
  const std::uint32_t central_offset =
      static_cast<std::uint32_t>(eocd[16]) | (static_cast<std::uint32_t>(eocd[17]) << 8) |
      (static_cast<std::uint32_t>(eocd[18]) << 16) | (static_cast<std::uint32_t>(eocd[19]) << 24);
  if (std::fseek(input, static_cast<long>(central_offset), SEEK_SET) != 0) {
    std::fclose(input);
    return false;
  }
  std::vector<ArchiveEntryInfo> entries;
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
    entries.push_back({std::move(name), crc, uncompressed});
  }
  std::fclose(input);
  *out = std::move(entries);
  return true;
}

bool archive_has_plain_marker(const std::string &archive_path, bool *cd_ok) {
  std::vector<ArchiveEntryInfo> entries;
  const bool read_ok = read_archive_central_directory(archive_path, &entries);
  if (cd_ok) {
    *cd_ok = read_ok;
  }
  if (!read_ok) {
    return false;
  }
  for (const ArchiveEntryInfo &entry : entries) {
    if (entry.path == kPlainContentMarkerName) {
      return true;
    }
  }
  return false;
}

bool archive_has_plain_format_entry(const std::string &archive_path, bool *cd_ok) {
  std::vector<ArchiveEntryInfo> entries;
  const bool read_ok = read_archive_central_directory(archive_path, &entries);
  if (cd_ok) {
    *cd_ok = read_ok;
  }
  if (!read_ok) {
    return false;
  }
  for (const ArchiveEntryInfo &entry : entries) {
    if (entry.path == kPlainFormatEntryName) {
      return true;
    }
  }
  return false;
}

int parse_plain_format_version(const std::string &content) {
  const std::size_t newline = content.find('\n');
  const std::string first_line = newline == std::string::npos ? content : content.substr(0, newline);
  std::size_t end = first_line.size();
  while (end > 0 && first_line[end - 1] >= '0' && first_line[end - 1] <= '9') {
    --end;
  }
  if (end == first_line.size()) {
    // No trailing digits on the first line at all.
    return 1;
  }
  const long value = std::strtol(first_line.substr(end).c_str(), nullptr, 10);
  return value >= 1 ? static_cast<int>(value) : 1;
}

bool entries_match_backup_archive(const std::vector<ArchiveEntryInfo> &folder_entries,
                                  const std::string &archive_path) {
  std::vector<ArchiveEntryInfo> archive_entries;
  if (!read_archive_central_directory(archive_path, &archive_entries)) {
    return false;
  }

  // Both sides are filtered here, not left to the caller: a live folder walk's sce_pfs churns
  // between sessions regardless of whether the save itself changed (comparison_entries), but an
  // OLD raw archive's central directory still lists sce_pfs - it was never excluded from what the
  // writer stores, only from what a comparison looks at. Filtering only the folder side would make
  // every such archive permanently unmatchable (a count mismatch that can never heal); filtering
  // both keeps them matchable against today's filtered folder walk.
  std::vector<ArchiveEntryInfo> folder_sorted = comparison_entries(folder_entries);
  std::vector<ArchiveEntryInfo> archive_sorted = comparison_entries(archive_entries);
  if (archive_sorted.size() != folder_sorted.size()) {
    return false;
  }

  // collect_files walks children in sorted order on both sides, but sort defensively so the
  // comparison never depends on traversal details.
  const auto by_path = [](const ArchiveEntryInfo &a, const ArchiveEntryInfo &b) {
    return a.path < b.path;
  };
  std::sort(folder_sorted.begin(), folder_sorted.end(), by_path);
  std::sort(archive_sorted.begin(), archive_sorted.end(), by_path);
  for (std::size_t i = 0; i < folder_sorted.size(); ++i) {
    if (folder_sorted[i].path != archive_sorted[i].path ||
        folder_sorted[i].crc32 != archive_sorted[i].crc32 ||
        folder_sorted[i].size != archive_sorted[i].size) {
      return false;
    }
  }
  return true;
}

std::uint64_t compute_folder_size(const std::string &folder_path, bool *ok,
                                  std::size_t *file_count) {
  std::uint64_t total = 0;
  if (file_count) {
    *file_count = 0;
  }
  const bool walked = add_directory_size(folder_path, &total, file_count);
  if (ok) {
    *ok = walked;
  }
  return total;
}

std::uint64_t archive_file_size(const std::string &archive_path, bool *ok) {
  struct stat info;
  const bool got = stat_path(archive_path, &info) && S_ISREG(info.st_mode);
  if (ok) {
    *ok = got;
  }
  return got ? static_cast<std::uint64_t>(info.st_size) : 0;
}

ArchiveReadResult read_backup_entry(const std::string &archive_path, const std::string &entry_path,
                                    std::size_t max_size) {
  ArchiveReadResult result;
  if (!is_safe_zip_entry_path(entry_path)) {
    result.error = "entry path is unsafe";
    return result;
  }
  FILE *zip = std::fopen(archive_path.c_str(), "rb");
  if (!zip) {
    result.error = "could not open archive";
    return result;
  }
  while (true) {
    std::uint32_t signature = 0;
    if (!read_u32(zip, &signature)) {
      result.error = "could not read archive";
      break;
    }
    if (signature == 0x02014b50 || signature == 0x06054b50) {
      result.error = "entry not found";
      break;
    }
    if (signature != 0x04034b50) {
      result.error = "archive header is invalid";
      break;
    }
    LocalZipHeader header;
    const bool read_header = read_local_header_after_signature(zip, &header);
    const bool store_shape =
        read_header && header.method == 0 && header.compressed_size == header.uncompressed_size;
    const bool deflate_shape =
        read_header && header.method == 8 && header.compressed_size < header.uncompressed_size;
    if (!read_header || (header.flags & 0x0008U) != 0 || (!store_shape && !deflate_shape) ||
        !is_safe_zip_entry_path(header.name)) {
      result.error = "archive entry is unsupported";
      break;
    }
    if (header.name != entry_path) {
      if (!skip_bytes(zip, header.compressed_size)) {
        result.error = "archive entry is truncated";
        break;
      }
      continue;
    }
    if (header.uncompressed_size > max_size) {
      result.error = "archive entry is too large";
      break;
    }
    result.data.resize(header.uncompressed_size);
    if (store_shape) {
      if (!result.data.empty() && !read_bytes(zip, result.data.data(), result.data.size())) {
        result.data.clear();
        result.error = "archive entry is truncated";
        break;
      }
    } else if (!inflate_entry_to_buffer(zip, header, &result.data)) {
      result.data.clear();
      result.error = "archive entry is corrupt";
      break;
    }
    if (update_crc32(0, result.data.data(), result.data.size()) != header.crc32) {
      result.data.clear();
      result.error = "archive entry checksum failed";
      break;
    }
    result.ok = true;
    break;
  }
  std::fclose(zip);
  return result;
}

bool remove_directory_tree(const std::string &path) {
  if (path.empty() || path == "/") {
    return false;
  }
  return remove_tree(path);
}

RestoreResult restore_backup_archive(const RestoreRequest &request) {
  const bool multi_target = !request.targets.empty();
  if (multi_target) {
    if (!tracked_paths_are_well_formed(request.targets)) {
      return restore_error("tracked paths are misconfigured");
    }
    for (const RestoreTarget &target : request.targets) {
      if (target.destination_path.empty() || target.destination_path == "/") {
        return restore_error("destination path is unsafe");
      }
    }
  } else if (request.destination_path.empty() || request.destination_path == "/") {
    return restore_error("destination path is unsafe");
  }

  FILE *zip = std::fopen(request.archive_path.c_str(), "rb");
  if (!zip) {
    return restore_error("could not open archive");
  }

  // In targets mode destination_path is not necessarily set (the caller maps several prefixes to
  // several directories instead), so the staging directory is keyed off the archive path.
  const std::string staging_path =
      (multi_target ? request.archive_path : request.destination_path) + ".restore-tmp";
  remove_tree(staging_path);
  const bool ok = ensure_directory(staging_path) &&
                  extract_archive_to_directory(zip, staging_path, UINT64_MAX, nullptr,
                                               request.progress);
  std::fclose(zip);
  if (!ok) {
    remove_tree(staging_path);
    return restore_error("could not restore archive");
  }

  if (multi_target) {
    for (const RestoreTarget &target : request.targets) {
      if (target.is_file) {
        // A file target replaces exactly one file - never a directory clear. The staged copy
        // moves over it; a prefix absent from the archive means the file was missing when the
        // backup was made, so removing the live one mirrors the backup the same way an empty
        // prefix leaves a directory target empty.
        const std::string staged_dir =
            target.prefix.empty() ? staging_path : join_path(staging_path, target.prefix);
        const std::string staged_file =
            join_path(staged_dir, path_basename(target.destination_path));
        if (is_regular_file(staged_file)) {
          const std::size_t slash = target.destination_path.rfind('/');
          if (slash != std::string::npos &&
              !ensure_directory(target.destination_path.substr(0, slash))) {
            remove_tree(staging_path);
            return restore_error("could not create destination folder");
          }
          if (std::rename(staged_file.c_str(), target.destination_path.c_str()) != 0) {
            remove_tree(staging_path);
            return restore_error("could not replace destination file");
          }
        } else {
          std::remove(target.destination_path.c_str());
        }
        continue;
      }
      if (!clear_directory_contents(target.destination_path)) {
        remove_tree(staging_path);
        return restore_error("could not clear destination save");
      }
      const std::string prefixed_staging =
          target.prefix.empty() ? staging_path : join_path(staging_path, target.prefix);
      // A prefix absent from the archive means that source had nothing to back up: the
      // destination stays cleared and empty, mirroring the backup rather than failing restore.
      if (is_directory(prefixed_staging) &&
          !move_directory_contents(prefixed_staging, target.destination_path)) {
        remove_tree(staging_path);
        return restore_error("could not replace destination save");
      }
    }
  } else {
    if (!clear_directory_contents(request.destination_path)) {
      remove_tree(staging_path);
      return restore_error("could not clear destination save");
    }
    if (!move_directory_contents(staging_path, request.destination_path)) {
      remove_tree(staging_path);
      return restore_error("could not replace destination save");
    }
  }
  remove_tree(staging_path);

  RestoreResult result;
  result.ok = true;
  return result;
}

RestoreResult extract_backup_archive_for_inspection(
    const std::string &archive_path, const std::string &destination_path,
    std::uint64_t max_total_bytes,
    const std::function<void(std::uint64_t, std::uint64_t)> &progress) {
  struct stat info {};
  if (destination_path.empty() || destination_path == "/" ||
      stat(destination_path.c_str(), &info) == 0) {
    return restore_error("inspection destination is unsafe or already exists");
  }
  FILE *zip = std::fopen(archive_path.c_str(), "rb");
  if (!zip) {
    return restore_error("could not open archive");
  }
  bool timestamps_uniform = false;
  std::uint64_t content_bytes = 0;
  // Pre-sum the entries this extraction will actually write (the skip-listed control entries
  // never land on disk), so progress can run in output mode - see extract_archive_to_directory.
  // An unreadable central directory just falls back to legacy archive-byte progress.
  std::uint64_t output_total = 0;
  if (progress) {
    std::vector<ArchiveEntryInfo> cd_entries;
    if (read_archive_central_directory(archive_path, &cd_entries)) {
      for (const ArchiveEntryInfo &entry : cd_entries) {
        if (entry.path != kPlainContentMarkerName && entry.path != kPlainFormatEntryName) {
          output_total += entry.size;
        }
      }
    }
  }
  const bool extracted = ensure_directory(destination_path) &&
                         extract_archive_to_directory(zip, destination_path, max_total_bytes,
                                                      &timestamps_uniform, progress,
                                                      &content_bytes, output_total);
  std::fclose(zip);
  RestoreResult result = extracted ? RestoreResult{true, {}, timestamps_uniform}
                                   : restore_error("could not inspect archive");
  if (extracted) {
    result.content_bytes = content_bytes;
  }
  if (!result.ok) {
    remove_tree(destination_path);
  }
  return result;
}

bool remove_backup_inspection_directory(const std::string &path) {
  if (path.empty() || path == "/") {
    return false;
  }
  struct stat info {};
  if (stat(path.c_str(), &info) != 0) {
    return errno == ENOENT;
  }
  return remove_tree(path);
}

} // namespace vsm
