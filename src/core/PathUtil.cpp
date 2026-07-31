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

std::string normalize_folder_title(const std::string &title) {
  std::string collapsed;
  collapsed.reserve(title.size());
  bool pending_space = false;
  for (const char value : title) {
    // An apostrophe or quote is inside a word; anything else unsafe stands between words, so it
    // yields a space. is_ascii_space folds the newlines some titles carry as well.
    const bool drops = value == '\'' || value == '"';
    const bool separates = !drops && (is_unsafe_path_character(value) || is_ascii_space(value));
    if (drops) {
      continue;
    }
    if (separates) {
      // Held rather than appended, so runs collapse and a trailing run never reaches the result.
      pending_space = !collapsed.empty();
      continue;
    }
    if (pending_space) {
      collapsed.push_back(' ');
      pending_space = false;
    }
    collapsed.push_back(value);
  }
  return collapsed;
}

std::string join_path(const std::string &parent, const std::string &child) {
  if (parent.empty() || parent.back() == '/') {
    return parent + child;
  }
  return parent + "/" + child;
}

} // namespace vsm
