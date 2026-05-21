#pragma once

#include <map>
#include <string>
#include <vector>

namespace vsm {

struct SfoMetadata {
  bool ok{};
  std::string error;
  std::map<std::string, std::string> strings;
};

SfoMetadata parse_sfo_metadata(const std::vector<unsigned char> &data);
SfoMetadata parse_sfo_metadata_file(const std::string &path);
std::string title_from_sfo_metadata(const SfoMetadata &metadata);

} // namespace vsm
