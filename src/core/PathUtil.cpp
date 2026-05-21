#include "core/PathUtil.hpp"

#include <algorithm>
#include <cctype>

namespace vsm {
namespace {

bool is_unsafe_path_character(char value) {
  switch (value) {
  case '\\':
  case '/':
  case ':':
  case '*':
  case '?':
  case '"':
  case '\'':
  case '<':
  case '>':
  case '|':
    return true;
  default:
    return false;
  }
}

bool is_ascii_space(char value) {
  return std::isspace(static_cast<unsigned char>(value)) != 0;
}

} // namespace

std::string normalize_path_component(const std::string &input) {
  std::string normalized = input;
  for (char &value : normalized) {
    if (is_unsafe_path_character(value)) {
      // Title names and save IDs eventually become both local directory names and Drive folder
      // names. Replacing separators and shell-hostile characters keeps one normalized component
      // from accidentally becoming multiple path levels on either side.
      value = '_';
    }
  }

  // Trimming avoids visually identical folders that differ only by padding, which is painful to
  // resolve with a controller-only UI.
  const auto first = std::find_if_not(normalized.begin(), normalized.end(), is_ascii_space);
  const auto last = std::find_if_not(normalized.rbegin(), normalized.rend(), is_ascii_space).base();
  if (first >= last) {
    return {};
  }

  return std::string(first, last);
}

} // namespace vsm
