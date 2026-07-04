#include "core/TextUtil.hpp"

namespace vsm {

std::string truncate_utf8_label(const std::string &text, std::size_t max_bytes) {
  if (text.size() <= max_bytes) {
    return text;
  }
  if (max_bytes <= 3) {
    return text.substr(0, max_bytes);
  }
  std::size_t cut = max_bytes - 3;
  // Continuation bytes look like 10xxxxxx; stepping back to the sequence start keeps the string
  // valid UTF-8.
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  return text.substr(0, cut) + "...";
}

bool needs_system_font(const std::string &text) {
  for (const char ch : text) {
    // UTF-8 lead bytes 0xE3 and above encode U+3000 and higher: kana, CJK, fullwidth forms.
    if (static_cast<unsigned char>(ch) >= 0xE3) {
      return true;
    }
  }
  return false;
}

} // namespace vsm
