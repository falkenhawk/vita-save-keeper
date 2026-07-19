#include "core/MultipartUpload.hpp"

#include <algorithm>

namespace vsm {

std::uint64_t MultipartRelatedBody::total_size() const {
  return prefix.size() + file_size + suffix.size();
}

MultipartRelatedBody build_multipart_related_body(const std::string &boundary,
                                                  const std::string &metadata_json,
                                                  const std::string &file_content_type,
                                                  std::uint64_t file_size) {
  MultipartRelatedBody body;
  body.prefix = "--" + boundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n" +
                metadata_json + "\r\n--" + boundary + "\r\nContent-Type: " + file_content_type +
                "\r\n\r\n";
  body.suffix = "\r\n--" + boundary + "--\r\n";
  body.file_size = file_size;
  return body;
}

MultipartChunk next_multipart_chunk(const MultipartRelatedBody &body, std::uint64_t body_offset,
                                    std::size_t capacity) {
  MultipartChunk chunk;

  const std::uint64_t file_begin = body.prefix.size();
  const std::uint64_t suffix_begin = file_begin + body.file_size;
  const std::uint64_t total = suffix_begin + body.suffix.size();
  if (body_offset >= total) {
    return chunk;
  }

  std::uint64_t region_remaining = 0;
  if (body_offset < file_begin) {
    chunk.source = MultipartChunk::Source::Prefix;
    chunk.source_offset = body_offset;
    region_remaining = file_begin - body_offset;
  } else if (body_offset < suffix_begin) {
    chunk.source = MultipartChunk::Source::File;
    chunk.source_offset = body_offset - file_begin;
    region_remaining = suffix_begin - body_offset;
  } else {
    chunk.source = MultipartChunk::Source::Suffix;
    chunk.source_offset = body_offset - suffix_begin;
    region_remaining = total - body_offset;
  }
  chunk.length = static_cast<std::size_t>(
      std::min<std::uint64_t>(region_remaining, capacity));
  return chunk;
}

} // namespace vsm
