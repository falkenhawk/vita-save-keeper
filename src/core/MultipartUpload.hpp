#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vsm {

// A multipart/related upload body split around the file payload, so the transport can stream the
// file from disk instead of materializing prefix + file + suffix in memory (a 52 MB save archive
// used to cost ~4x its size in transient string copies, aborting the app on out-of-memory).
// prefix + <file bytes> + suffix is byte-identical to the previously in-RAM body.
struct MultipartRelatedBody {
  std::string prefix;
  std::string suffix;
  std::uint64_t file_size{};

  std::uint64_t total_size() const;
};

MultipartRelatedBody build_multipart_related_body(const std::string &boundary,
                                                  const std::string &metadata_json,
                                                  const std::string &file_content_type,
                                                  std::uint64_t file_size);

// The next contiguous slice of the virtual body [prefix | file | suffix] to serve from a given
// absolute offset. A chunk never spans two regions - the reader switches data sources between
// calls, and short reads are fine for curl-style read callbacks.
struct MultipartChunk {
  enum class Source { Prefix, File, Suffix, End };
  Source source{Source::End};
  // Offset within the chunk's own region (for File: the position to read the file at).
  std::uint64_t source_offset{};
  std::size_t length{};
};

MultipartChunk next_multipart_chunk(const MultipartRelatedBody &body, std::uint64_t body_offset,
                                    std::size_t capacity);

} // namespace vsm
