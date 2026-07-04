#pragma once

#include <cstddef>
#include <string>

namespace vsm {

// Truncates to at most max_bytes bytes, appending "..." when something was cut. Never splits a
// UTF-8 sequence, so truncated CJK titles stay renderable instead of ending in garbage bytes.
std::string truncate_utf8_label(const std::string &text, std::size_t max_bytes);

// True when the text contains codepoints from U+3000 upwards (kana, CJK ideographs, fullwidth
// forms), which the bundled Latin UI font cannot render and the system font can.
bool needs_system_font(const std::string &text);

} // namespace vsm
