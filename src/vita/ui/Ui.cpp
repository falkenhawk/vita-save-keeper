#include "vita/ui/Ui.hpp"

#include "core/BackupList.hpp"
#include "core/BackupName.hpp"
#include "core/GoogleAuth.hpp"
#include "core/GridWindow.hpp"
#include "core/TextUtil.hpp"

#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/threadmgr.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <qrcodegen.hpp>
#include <string>
#include <vector>
#include <vita2d.h>

namespace vsm::vita {
namespace {

constexpr unsigned int kColorBackground = RGBA8(8, 13, 21, 255);
constexpr unsigned int kColorHeader = RGBA8(17, 24, 36, 255);
constexpr unsigned int kColorPanel = RGBA8(16, 24, 36, 255);
constexpr unsigned int kColorPanelAlt = RGBA8(25, 37, 55, 255);
constexpr unsigned int kColorAccent = RGBA8(56, 189, 248, 255);
constexpr unsigned int kColorAccentSoft = RGBA8(56, 189, 248, 40);
constexpr unsigned int kColorText = RGBA8(232, 238, 246, 255);
constexpr unsigned int kColorMuted = RGBA8(166, 178, 198, 255);
constexpr unsigned int kColorSuccess = RGBA8(74, 222, 128, 255);
constexpr unsigned int kColorError = RGBA8(248, 113, 113, 255);
constexpr unsigned int kColorPendingDot = RGBA8(251, 191, 36, 255);
constexpr unsigned int kColorIdleDot = RGBA8(100, 116, 139, 255);
constexpr const char *kBundledFontPath = "app0:sce_sys/resources/Roboto-Regular.ttf";

constexpr unsigned int kTextSizeTiny = 15;
constexpr unsigned int kTextSizeSmall = 17;
constexpr unsigned int kTextSizeNormal = 18;
constexpr unsigned int kTextSizeTitle = 22;
constexpr unsigned int kTextSizeCode = 38;

// Slot-details description pane: scroll clamping (details_max_scroll) and rendering
// (draw_slot_details) must share these, or the last line becomes unreachable or overscrolls.
constexpr int kDetailsPaneWidth = 580;
constexpr int kVisibleDetailLines = 10;

// ISO timestamps are stored as "YYYY-MM-DDTHH:MM:SS"; the details and list rows show the date and
// time separated by a space instead of the 'T'.
std::string format_save_datetime_spaced(const SaveDateTime &value) {
  std::string text = format_save_datetime(value);
  if (text.size() > 10) {
    // One space reads cramped between the date and time digit blocks - the font's space advance
    // is roughly half a digit cell - so the separator gets two.
    text.replace(10, 1, "  ");
  }
  return text;
}

// Human-readable byte size for the details pane: whole bytes under 1 KB, otherwise one decimal
// in binary (1024-based) KB/MB/GB.
std::string format_bytes(std::uint64_t bytes) {
  constexpr double kUnit = 1024.0;
  if (bytes < 1024) {
    return std::to_string(bytes) + " B";
  }
  static const char *const kSuffixes[] = {"KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int index = -1;
  do {
    value /= kUnit;
    ++index;
  } while (value >= kUnit && index < 3);
  // %.1f rounds, so 1023.96 KB would print as "1024.0 KB"; roll such values into the next unit.
  if (value >= kUnit - 0.05 && index < 3) {
    value /= kUnit;
    ++index;
  }
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f %s", value, kSuffixes[index]);
  return text;
}

// A small rotating-dot circle spinner for a value still resolving (a save time read through a
// mount). Eight dots sit on a fixed ring; a bright head sweeps around with the trailing dots
// fading behind it. Driven by the per-frame counter, ~1.9 revolutions/second.
void draw_circle_spinner(float cx, float cy, unsigned int frame, unsigned int base_color) {
  constexpr int kDots = 8;
  // Fixed ring positions (radius 6), starting at the top and going clockwise.
  static const float kOffsets[kDots][2] = {
      {0.0f, -6.0f},  {4.24f, -4.24f}, {6.0f, 0.0f},  {4.24f, 4.24f},
      {0.0f, 6.0f},   {-4.24f, 4.24f}, {-6.0f, 0.0f}, {-4.24f, -4.24f},
  };
  const int head = static_cast<int>((frame / 4U) % kDots);
  for (int i = 0; i < kDots; ++i) {
    const int trail = (head - i + kDots) % kDots;  // 0 = head (brightest), higher = further behind
    unsigned int alpha = 255U - static_cast<unsigned int>(trail) * 26U;
    if (alpha < 40U) {
      alpha = 40U;
    }
    const unsigned int color = (base_color & 0x00FFFFFFu) | (alpha << 24);
    vita2d_draw_fill_circle(cx + kOffsets[i][0], cy + kOffsets[i][1], 2.0f, color);
  }
}

enum class ButtonSymbol {
  Circle,
  Cross,
  Square,
  Triangle,
  Select,
  Start,
  // Text-only footer entry with no glyph: a group label like "Delete:" that introduces the
  // buttons to its right.
  Label,
};

void draw_text(const FontSet &fonts, int x, int y, unsigned int color, unsigned int size,
               const char *text) {
  vita2d_font *font = size < FontSet::kMaxSize ? fonts.by_size[size] : nullptr;
  // The bundled Latin font has no kana/CJK glyphs, so Japanese titles route to the system PGF
  // font, unscaled (fractional PGF scaling looked bad on hardware).
  const bool use_fallback = !font || (fonts.fallback && needs_system_font(text));
  if (!use_fallback) {
    vita2d_font_draw_text(font, x, y, color, size, text);
  } else if (fonts.fallback) {
    vita2d_pgf_draw_text(fonts.fallback, x, y, color, 1.0f, text);
  } else if (font) {
    vita2d_font_draw_text(font, x, y, color, size, text);
  }
}

int text_width(const FontSet &fonts, unsigned int size, const char *text) {
  vita2d_font *font = size < FontSet::kMaxSize ? fonts.by_size[size] : nullptr;
  const bool use_fallback = !font || (fonts.fallback && needs_system_font(text));
  if (!use_fallback) {
    return vita2d_font_text_width(font, size, text);
  }
  if (fonts.fallback) {
    return vita2d_pgf_text_width(fonts.fallback, 1.0f, text);
  }
  return font ? vita2d_font_text_width(font, size, text) : 0;
}

// The Latin font's digits are proportional ('4' is a few pixels wider than '3'), so identical
// "YYYY-MM-DD HH:MM:SS" patterns stacked in the slot list end up with ragged left edges and
// wobbling interior columns. draw_datetime_tabular draws every digit centered in a fixed cell
// (the widest digit's advance) so each timestamp lays out identically and columns align.
//
// Advances are measured in context - width("0c0") - width("00") - because a lone glyph's width
// includes both side bearings, which do not apply between characters; cells sized that way came
// out ~2px too wide and the dates looked letter-spaced. Cached because fonts never change after
// init and this runs for every visible slot row each frame.
struct TabularMetrics {
  int advance[128] {};
  int cell {};
};

int tabular_advance(const FontSet &fonts, unsigned int size, TabularMetrics *metrics, char c) {
  const unsigned char index = static_cast<unsigned char>(c);
  if (index >= 128) {
    // Timestamps are pure ASCII; anything else falls back to the lone-glyph width.
    const char one[2] = {c, '\0'};
    return text_width(fonts, size, one);
  }
  if (metrics->advance[index] == 0) {
    const char pair[3] = {'0', '0', '\0'};
    const char triple[4] = {'0', c, '0', '\0'};
    metrics->advance[index] =
        std::max(1, text_width(fonts, size, triple) - text_width(fonts, size, pair));
  }
  return metrics->advance[index];
}

TabularMetrics &tabular_metrics(const FontSet &fonts, unsigned int size) {
  static TabularMetrics cache[FontSet::kMaxSize];
  TabularMetrics &metrics = cache[size < FontSet::kMaxSize ? size : 0];
  if (metrics.cell == 0) {
    // '0' is the canonical tabular width. This font's digit advances genuinely range ~7-10px, so
    // sizing cells to the widest digit letter-spaced every date; in a '0'-wide cell the odd wider
    // digit just tucks in (glyph ink is narrower than its advance) and the tracking stays natural.
    metrics.cell = tabular_advance(fonts, size, &metrics, '0');
  }
  return metrics;
}

void draw_datetime_tabular(const FontSet &fonts, int right_x, int y, unsigned int color,
                           unsigned int size, const std::string &text) {
  TabularMetrics &metrics = tabular_metrics(fonts, size);
  int total = 0;
  for (const char c : text) {
    total += (c >= '0' && c <= '9') ? metrics.cell : tabular_advance(fonts, size, &metrics, c);
  }
  int x = right_x - total;
  for (const char c : text) {
    const char one[2] = {c, '\0'};
    const int advance = tabular_advance(fonts, size, &metrics, c);
    if (c >= '0' && c <= '9') {
      draw_text(fonts, x + (metrics.cell - advance) / 2, y, color, size, one);
      x += metrics.cell;
    } else {
      draw_text(fonts, x, y, color, size, one);
      x += advance;
    }
  }
}

const char *platform_label(SavePlatform platform) {
  switch (platform) {
  case SavePlatform::Vita:
    return "Vita";
  case SavePlatform::GameCard:
    return "Game Card";
  case SavePlatform::Psp:
    return "PSP";
  default:
    return "Save";
  }
}

std::string truncate_label(const std::string &text, std::size_t max_length) {
  return truncate_utf8_label(text, max_length);
}

const SaveRecord *selected_visible_record(const UiState &state) {
  if (!state.visible_saves || state.visible_saves->empty()) {
    return nullptr;
  }
  const std::vector<std::size_t> &visible = *state.visible_saves;
  return &(*state.saves)[visible[state.selected_save % visible.size()]];
}

std::string title_id_label(const SaveRecord &save) {
  if (!save.title_id.empty() && save.title_id != save.id) {
    return save.title_id + " | " + save.id;
  }
  if (!save.title_id.empty()) {
    return save.title_id;
  }
  return save.id;
}

std::string placeholder_text(const SaveRecord &save) {
  const std::string &source = save.display_name.empty() ? save.id : save.display_name;
  std::string result;
  for (char value : source) {
    if (std::isalnum(static_cast<unsigned char>(value))) {
      result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(value))));
      if (result.size() == 2) {
        break;
      }
    }
  }
  return result.empty() ? "SK" : result;
}

// Strips the scheme and www. so the on-screen link stays short enough to type from the couch.
std::string display_url(const std::string &url) {
  std::string result = url;
  const std::string scheme = "https://";
  if (result.compare(0, scheme.size(), scheme) == 0) {
    result.erase(0, scheme.size());
  }
  const std::string www = "www.";
  if (result.compare(0, www.size(), www) == 0) {
    result.erase(0, www.size());
  }
  return result;
}

void draw_qr_code(const std::string &value, int x, int y, int module_size) {
  if (value.empty()) {
    return;
  }

  const qrcodegen::QrCode qr =
      qrcodegen::QrCode::encodeText(value.c_str(), qrcodegen::QrCode::Ecc::LOW);
  // Scanners rely on the light quiet zone around the code; without it phone cameras regularly
  // refuse to lock onto codes rendered on dark backgrounds.
  constexpr int kQuietZone = 2;
  const int size = qr.getSize();
  vita2d_draw_rectangle(x, y, (size + kQuietZone * 2) * module_size,
                        (size + kQuietZone * 2) * module_size,
                        RGBA8(255, 255, 255, 255));

  for (int row = 0; row < size; ++row) {
    for (int col = 0; col < size; ++col) {
      if (qr.getModule(col, row)) {
        vita2d_draw_rectangle(x + (col + kQuietZone) * module_size,
                              y + (row + kQuietZone) * module_size, module_size, module_size,
                              RGBA8(15, 23, 42, 255));
      }
    }
  }
}

void draw_texture_fit(vita2d_texture *texture, int x, int y, int size) {
  if (!texture) {
    return;
  }

  const unsigned int width = vita2d_texture_get_width(texture);
  const unsigned int height = vita2d_texture_get_height(texture);
  if (width == 0 || height == 0) {
    return;
  }

  const float scale =
      static_cast<float>(size) / static_cast<float>(std::max(width, height));
  const float draw_width = static_cast<float>(width) * scale;
  const float draw_height = static_cast<float>(height) * scale;
  vita2d_draw_texture_scale(texture, x + (size - draw_width) / 2.0f,
                            y + (size - draw_height) / 2.0f, scale, scale);
}

void draw_placeholder_icon(const FontSet &fonts, const SaveRecord &save, int x, int y, int size,
                           bool selected) {
  const unsigned int tile = selected ? RGBA8(29, 49, 69, 255) : RGBA8(35, 49, 69, 255);
  vita2d_draw_rectangle(x, y, size, size, tile);
  const int card_x = x + size / 5;
  const int card_y = y + size / 6;
  const int card_w = size - (size / 5) * 2;
  const int card_h = size / 2;
  vita2d_draw_rectangle(card_x, card_y, card_w, card_h, RGBA8(12, 20, 32, 255));
  vita2d_draw_rectangle(card_x, card_y, card_w, 8, kColorAccent);
  vita2d_draw_rectangle(card_x + 10, card_y + 22, card_w - 20, 7, RGBA8(232, 238, 246, 255));
  vita2d_draw_rectangle(card_x + 14, card_y + 38, card_w - 28, 12, RGBA8(21, 33, 49, 255));
  const std::string text = placeholder_text(save);
  draw_text(fonts, x + 18, y + size - 14,
            selected ? kColorText : RGBA8(198, 210, 226, 255), kTextSizeNormal, text.c_str());
}

void draw_button_shape(int x, int y, ButtonSymbol symbol, unsigned int color) {
  switch (symbol) {
  case ButtonSymbol::Circle:
    vita2d_draw_fill_circle(x + 8, y + 8, 8, color);
    vita2d_draw_fill_circle(x + 8, y + 8, 5, RGBA8(3, 7, 18, 230));
    break;
  case ButtonSymbol::Cross:
    vita2d_draw_line(x + 2, y + 2, x + 14, y + 14, color);
    vita2d_draw_line(x + 14, y + 2, x + 2, y + 14, color);
    break;
  case ButtonSymbol::Square:
    vita2d_draw_line(x + 1, y + 1, x + 15, y + 1, color);
    vita2d_draw_line(x + 15, y + 1, x + 15, y + 15, color);
    vita2d_draw_line(x + 15, y + 15, x + 1, y + 15, color);
    vita2d_draw_line(x + 1, y + 15, x + 1, y + 1, color);
    break;
  case ButtonSymbol::Triangle:
    vita2d_draw_line(x + 8, y, x + 16, y + 15, color);
    vita2d_draw_line(x + 16, y + 15, x, y + 15, color);
    vita2d_draw_line(x, y + 15, x + 8, y, color);
    break;
  case ButtonSymbol::Select:
  case ButtonSymbol::Start:
    // Handled in draw_hint as labeled pills; the abstract line glyphs read poorly on hardware.
    break;
  }
}

// The footer band spans y 508..544; everything below is laid out around its center line at 526.
constexpr int kFooterSymbolTop = 518;
constexpr int kFooterBaseline = 532;

// Lays hints out right-to-left so the primary action always sits at the right edge, the way the
// system UI arranges its button hints. dim_prefix/dim_suffix render smaller and dimmer than the
// label: gesture qualifiers like "(hold)" that should read as a note, not part of the action.
struct HintSpec {
  ButtonSymbol symbol;
  const char *label;
  const char *dim_prefix;
  const char *dim_suffix;
};

int hint_width(const FontSet &fonts, const HintSpec &hint) {
  int width = hint.label ? text_width(fonts, kTextSizeSmall, hint.label) : 0;
  if (hint.dim_prefix) {
    width += text_width(fonts, kTextSizeTiny, hint.dim_prefix) + 6;
  }
  if (hint.dim_suffix) {
    width += 6 + text_width(fonts, kTextSizeTiny, hint.dim_suffix);
  }
  if (hint.symbol == ButtonSymbol::Label) {
    return width;  // text only, no glyph box
  }
  if (hint.symbol == ButtonSymbol::Select || hint.symbol == ButtonSymbol::Start) {
    const char *pill_text = hint.symbol == ButtonSymbol::Start ? "START" : "SEL";
    return text_width(fonts, kTextSizeTiny, pill_text) + 12 + 8 + width;
  }
  return 22 + width;
}

void draw_hint(const FontSet &fonts, int x, const HintSpec &hint) {
  int text_x = x + 22;
  if (hint.symbol == ButtonSymbol::Label) {
    // No glyph: the label text starts at the entry's left edge.
    text_x = x;
  } else if (hint.symbol == ButtonSymbol::Select || hint.symbol == ButtonSymbol::Start) {
    const char *pill_text = hint.symbol == ButtonSymbol::Start ? "START" : "SEL";
    // Size the pill from the measured text so the label never spills past its edge.
    const int pill_w = text_width(fonts, kTextSizeTiny, pill_text) + 12;
    vita2d_draw_rectangle(x, 516, pill_w, 20, RGBA8(255, 255, 255, 30));
    draw_text(fonts, x + 6, 531, kColorMuted, kTextSizeTiny, pill_text);
    text_x = x + pill_w + 8;
  } else {
    draw_button_shape(x, kFooterSymbolTop, hint.symbol, kColorMuted);
  }
  if (hint.dim_prefix) {
    draw_text(fonts, text_x, kFooterBaseline, kColorIdleDot, kTextSizeTiny, hint.dim_prefix);
    text_x += text_width(fonts, kTextSizeTiny, hint.dim_prefix) + 6;
  }
  if (hint.label) {
    // A group label is faux-bold (the Regular cut drawn twice, offset 1px) so it reads as a
    // heading over the buttons it introduces, not as another button hint. Same muted tone.
    const bool group_label = hint.symbol == ButtonSymbol::Label;
    draw_text(fonts, text_x, kFooterBaseline, kColorMuted, kTextSizeSmall, hint.label);
    if (group_label) {
      draw_text(fonts, text_x + 1, kFooterBaseline, kColorMuted, kTextSizeSmall, hint.label);
    }
    text_x += text_width(fonts, kTextSizeSmall, hint.label);
  }
  if (hint.dim_suffix) {
    draw_text(fonts, text_x + 6, kFooterBaseline, kColorIdleDot, kTextSizeTiny, hint.dim_suffix);
  }
}

// Returns the leftmost x it drew to, so a caller can fit other footer content beside the hints.
int draw_hints_right_aligned(const FontSet &fonts, const HintSpec *hints, int count) {
  // A group label and every hint to its right form one tight cluster - the "Delete: Local Cloud
  // Both" options group - set close together, while the gap that separates the cluster from the
  // rest of the footer (e.g. Cancel) stays the standard width.
  int group_start = count;
  for (int i = 0; i < count; ++i) {
    if (hints[i].symbol == ButtonSymbol::Label) {
      group_start = i;
      break;
    }
  }
  int x = 936;
  int leftmost = x;
  for (int i = count - 1; i >= 0; --i) {
    x -= hint_width(fonts, hints[i]);
    draw_hint(fonts, x, hints[i]);
    leftmost = x;
    const bool inside_group = (i - 1) >= group_start;  // hint[i-1] and hint[i] both in the group
    x -= inside_group ? 16 : 36;
  }
  return leftmost;
}

std::string format_minutes_seconds(int total_seconds) {
  if (total_seconds < 0) {
    total_seconds = 0;
  }
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%d:%02d", total_seconds / 60, total_seconds % 60);
  return buffer;
}

// Drive state is the only thing a row marks: a card copy is the normal case and stays unmarked.
// Sky cloud + check = synced (card and Drive); amber cloud + down arrow = lives only on Drive
// (Select downloads a card copy, restore fetches it automatically). The silhouette spans
// x+1..x+27 by y..y+17, filling the 36px row without growing it; placed at x 902 it ends at 929,
// inside the row's right edge at 936.
// The cloud body is composed so no flat rectangle edge ever forms the outline: the base rect
// spans only between the end circles' centers, so the circles round the left, right, and both
// bottom corners, and two larger puffs shape the top. (The first attempt drew the rect wider
// than the circles, which made the silhouette read as chopped off on the right and bottom.)
void draw_cloud_body(int x, int y, unsigned int color) {
  vita2d_draw_rectangle(x + 6, y + 7, 16, 10, color);
  vita2d_draw_fill_circle(x + 6, y + 12, 5, color);
  vita2d_draw_fill_circle(x + 22, y + 12, 5, color);
  vita2d_draw_fill_circle(x + 12, y + 7, 7, color);
  vita2d_draw_fill_circle(x + 19.5f, y + 8.5f, 5.5f, color);
}

void draw_cloud_synced_glyph(int x, int y) {
  draw_cloud_body(x, y, kColorAccent);
  // The check is cut out in the panel color; vita2d lines are 1px, so each stroke is tripled
  // with 1px offsets to read as a ~3px mark on hardware.
  const unsigned int mark = RGBA8(15, 23, 42, 255);
  for (int off = -1; off <= 1; ++off) {
    vita2d_draw_line(x + 9, y + 11 + off, x + 12.5f, y + 14.5f + off, mark);
    vita2d_draw_line(x + 12.5f, y + 14.5f + off, x + 19.5f, y + 7 + off, mark);
  }
}

void draw_cloud_drive_only_glyph(int x, int y) {
  draw_cloud_body(x, y, kColorPendingDot);
  // Down arrow: stem plus a stacked-rect arrowhead (vita2d has no filled-triangle primitive).
  const unsigned int mark = RGBA8(15, 23, 42, 255);
  vita2d_draw_rectangle(x + 13, y + 3, 3, 7, mark);
  vita2d_draw_rectangle(x + 10, y + 10, 9, 2, mark);
  vita2d_draw_rectangle(x + 12, y + 12, 5, 2, mark);
  vita2d_draw_rectangle(x + 14, y + 14, 1, 2, mark);
}

// The IME dialog wants UTF-16 in and hands UTF-16 back; the rest of the app is UTF-8. Both
// helpers handle supplementary-plane codepoints (surrogate pairs) so pasted symbols survive the
// round trip instead of corrupting the file name.
void utf8_to_utf16(const std::string &utf8, SceWChar16 *out, std::size_t out_capacity) {
  std::size_t out_index = 0;
  std::size_t i = 0;
  while (i < utf8.size() && out_index + 1 < out_capacity) {
    const unsigned char lead = static_cast<unsigned char>(utf8[i]);
    std::uint32_t codepoint = 0xFFFD;
    std::size_t length = 1;
    if (lead < 0x80) {
      codepoint = lead;
    } else if ((lead & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      codepoint = static_cast<std::uint32_t>(lead & 0x1F) << 6 |
                  (static_cast<unsigned char>(utf8[i + 1]) & 0x3F);
      length = 2;
    } else if ((lead & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
      codepoint = static_cast<std::uint32_t>(lead & 0x0F) << 12 |
                  static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 6 |
                  (static_cast<unsigned char>(utf8[i + 2]) & 0x3F);
      length = 3;
    } else if ((lead & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
      codepoint = static_cast<std::uint32_t>(lead & 0x07) << 18 |
                  static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 1]) & 0x3F) << 12 |
                  static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 2]) & 0x3F) << 6 |
                  (static_cast<unsigned char>(utf8[i + 3]) & 0x3F);
      length = 4;
    }
    if (codepoint >= 0x10000) {
      if (out_index + 2 >= out_capacity) {
        break;
      }
      const std::uint32_t offset = codepoint - 0x10000;
      out[out_index++] = static_cast<SceWChar16>(0xD800 | (offset >> 10));
      out[out_index++] = static_cast<SceWChar16>(0xDC00 | (offset & 0x3FF));
    } else {
      out[out_index++] = static_cast<SceWChar16>(codepoint);
    }
    i += length;
  }
  out[out_index] = 0;
}

std::string utf16_to_utf8(const SceWChar16 *utf16) {
  std::string out;
  for (std::size_t i = 0; utf16[i] != 0; ++i) {
    std::uint32_t codepoint = utf16[i];
    if (codepoint >= 0xD800 && codepoint <= 0xDBFF && utf16[i + 1] >= 0xDC00 &&
        utf16[i + 1] <= 0xDFFF) {
      codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (utf16[i + 1] - 0xDC00);
      ++i;
    }
    if (codepoint < 0x80) {
      out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
      out += static_cast<char>(0xC0 | (codepoint >> 6));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
      out += static_cast<char>(0xE0 | (codepoint >> 12));
      out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (codepoint >> 18));
      out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
  }
  return out;
}

} // namespace

bool Ui::initialize() {
  if (vita2d_init() < 0) {
    return false;
  }

  vita2d_set_clear_color(kColorBackground);
  const unsigned int sizes[] = {kTextSizeTiny, kTextSizeSmall, kTextSizeNormal, kTextSizeTitle,
                                kTextSizeCode};
  bool any_font = false;
  for (const unsigned int size : sizes) {
    fonts_.by_size[size] = vita2d_load_font_file(kBundledFontPath);
    any_font = any_font || fonts_.by_size[size] != nullptr;
  }
  // The system PGF font needs no bundled file and covers kana/CJK, which the bundled Latin TTF
  // does not; it always loads as the fallback for Japanese titles (and for everything if the
  // TTF itself failed to load).
  fonts_.fallback = vita2d_load_default_pgf();

  cloud_synced_tex_ = vita2d_load_PNG_file("app0:sce_sys/resources/cloud-synced.png");
  cloud_drive_only_tex_ = vita2d_load_PNG_file("app0:sce_sys/resources/cloud-drive-only.png");
  cloud_local_only_tex_ = vita2d_load_PNG_file("app0:sce_sys/resources/cloud-local-only.png");

  return any_font || fonts_.fallback != nullptr;
}

void Ui::shutdown() {
  if (cloud_synced_tex_) {
    vita2d_free_texture(cloud_synced_tex_);
    cloud_synced_tex_ = nullptr;
  }
  if (cloud_drive_only_tex_) {
    vita2d_free_texture(cloud_drive_only_tex_);
    cloud_drive_only_tex_ = nullptr;
  }
  if (cloud_local_only_tex_) {
    vita2d_free_texture(cloud_local_only_tex_);
    cloud_local_only_tex_ = nullptr;
  }
  for (auto &entry : icon_cache_) {
    if (entry.second) {
      vita2d_free_texture(entry.second);
    }
  }
  icon_cache_.clear();

  for (unsigned int size = 0; size < FontSet::kMaxSize; ++size) {
    if (fonts_.by_size[size]) {
      vita2d_free_font(fonts_.by_size[size]);
      fonts_.by_size[size] = nullptr;
    }
  }
  if (fonts_.fallback) {
    vita2d_free_pgf(fonts_.fallback);
    fonts_.fallback = nullptr;
  }
  vita2d_fini();
}

int Ui::measure_text(unsigned int size, const char *text) const {
  return text_width(fonts_, size, text);
}

std::string Ui::fit_text(unsigned int size, const std::string &text, int max_width) const {
  if (measure_text(size, text.c_str()) <= max_width) {
    return text;
  }
  // Shrink whole UTF-8 codepoints until the ellipsized text fits the given pixel width; byte
  // counts under-use the pane for Latin titles and over-use it for CJK ones.
  std::string cut = text;
  while (!cut.empty()) {
    std::size_t last = cut.size() - 1;
    while (last > 0 && (static_cast<unsigned char>(cut[last]) & 0xC0) == 0x80) {
      --last;
    }
    cut.erase(last);
    const std::string candidate = cut + "...";
    if (measure_text(size, candidate.c_str()) <= max_width) {
      return candidate;
    }
  }
  return "...";
}

std::vector<std::string> Ui::wrap_text(unsigned int size, const std::string &text,
                                       int max_width) const {
  std::vector<std::string> lines;
  std::size_t paragraph_start = 0;
  while (paragraph_start <= text.size()) {
    const std::size_t newline = text.find('\n', paragraph_start);
    const std::size_t paragraph_end = newline == std::string::npos ? text.size() : newline;
    const std::string paragraph = text.substr(paragraph_start, paragraph_end - paragraph_start);
    std::string line;
    std::size_t cursor = 0;

    while (cursor < paragraph.size()) {
      while (cursor < paragraph.size() &&
             (paragraph[cursor] == ' ' || paragraph[cursor] == '\t')) {
        ++cursor;
      }
      const std::size_t word_start = cursor;
      while (cursor < paragraph.size() && paragraph[cursor] != ' ' &&
             paragraph[cursor] != '\t') {
        ++cursor;
      }
      std::string word = paragraph.substr(word_start, cursor - word_start);
      if (word.empty()) {
        continue;
      }

      const std::string candidate = line.empty() ? word : line + " " + word;
      if (measure_text(size, candidate.c_str()) <= max_width) {
        line = candidate;
        continue;
      }
      if (!line.empty()) {
        lines.push_back(line);
        line.clear();
      }

      // A single token may be wider than the pane (URLs and CJK commonly have no ASCII spaces).
      // Split only at UTF-8 codepoint boundaries so every rendered line remains valid text.
      while (!word.empty() && measure_text(size, word.c_str()) > max_width) {
        std::string chunk;
        std::size_t split = 0;
        while (split < word.size()) {
          std::size_t next = split + 1;
          while (next < word.size() &&
                 (static_cast<unsigned char>(word[next]) & 0xC0) == 0x80) {
            ++next;
          }
          const std::string grown = word.substr(0, next);
          if (!chunk.empty() && measure_text(size, grown.c_str()) > max_width) {
            break;
          }
          chunk = grown;
          split = next;
        }
        if (split == 0) {
          split = 1;
          while (split < word.size() &&
                 (static_cast<unsigned char>(word[split]) & 0xC0) == 0x80) {
            ++split;
          }
          chunk = word.substr(0, split);
        }
        lines.push_back(chunk);
        word.erase(0, split);
      }
      line = word;
    }

    if (!line.empty() || paragraph.empty()) {
      lines.push_back(line);
    }
    if (newline == std::string::npos) {
      break;
    }
    paragraph_start = newline + 1;
  }
  return lines;
}

std::string Ui::fit_quoted_name(const std::string &prefix, const std::string &name,
                                const std::string &suffix, unsigned int size, int max_width,
                                bool quote) const {
  const char *q = quote ? "\"" : "";
  std::string candidate = prefix + q + name + q + suffix;
  if (measure_text(size, candidate.c_str()) <= max_width) {
    return candidate;
  }
  std::string cut = name;
  while (!cut.empty()) {
    std::size_t last = cut.size() - 1;
    while (last > 0 && (static_cast<unsigned char>(cut[last]) & 0xC0) == 0x80) {
      --last;
    }
    cut.erase(last);
    candidate = prefix + q + cut + "..." + q + suffix;
    if (measure_text(size, candidate.c_str()) <= max_width) {
      return candidate;
    }
  }
  return prefix + q + "..." + q + suffix;
}

std::string Ui::compose_status_with_name(const std::string &prefix, const std::string &name,
                                         const std::string &suffix, int max_width) const {
  // The default matches the 380px budget draw_status_line renders with; the details screen's
  // footer line has more room and passes its own width.
  return fit_quoted_name(prefix, name, suffix, kTextSizeSmall, max_width > 0 ? max_width : 380);
}

std::string Ui::compose_modal_label(const std::string &prefix, const std::string &name,
                                    const std::string &suffix) const {
  // draw_busy renders the title at kTextSizeNormal inside the 400px box, text inset 24px a side.
  return fit_quoted_name(prefix, name, suffix, kTextSizeNormal, 400 - 48);
}

vita2d_texture *Ui::load_icon_texture(const std::string &path) {
  if (path.empty()) {
    return nullptr;
  }

  const auto cached = icon_cache_.find(path);
  if (cached != icon_cache_.end()) {
    return cached->second;
  }

  vita2d_texture *texture = vita2d_load_PNG_file(path.c_str());
  icon_cache_[path] = texture;
  return texture;
}

void Ui::draw(const UiState &state) {
  ++frame_counter_;
  last_state_ = state;
  has_last_state_ = true;
  vita2d_start_drawing();
  vita2d_clear_screen();

  if (state.slot_details && state.slot_details->open) {
    draw_slot_details(*state.slot_details, state.enter_is_cross, state.status_message,
                      state.status_kind, state.restore_confirmation_pending,
                      state.duplicate_backup_confirmation_pending);
  } else {
    draw_header(state);
    draw_title_grid(state);
    draw_backup_panel(state);
    draw_footer(state);
  }

  if (state.google_setup_prompt != GoogleSetupPrompt::None) {
    draw_google_setup_prompt(state);
  }

  vita2d_end_drawing();
  vita2d_swap_buffers();
}

void Ui::draw_presence_glyph(int x, int y, bool on_card, bool in_cloud) {
  vita2d_texture *glyph = nullptr;
  if (in_cloud) {
    glyph = on_card ? cloud_synced_tex_ : cloud_drive_only_tex_;
  } else if (on_card) {
    glyph = cloud_local_only_tex_;
  }
  if (glyph) {
    vita2d_draw_texture(glyph, static_cast<float>(x), static_cast<float>(y));
  } else if (in_cloud) {
    // Primitive fallback if a texture failed to load (there is none for local-only).
    if (on_card) {
      draw_cloud_synced_glyph(x, y);
    } else {
      draw_cloud_drive_only_glyph(x, y);
    }
  }
}

void Ui::draw_modal_backdrop() {
  // The previous frame, dimmed, behind a modal - mirroring draw()'s top-level branch. The
  // details view draws from a UiState that carries only the slot_details pointer; its grid
  // pointers are null and must never be dereferenced here (the Download & Inspect busy modal,
  // the first ever shown over Save Details, crashed on exactly that). Before the first frame
  // there is nothing to repaint and the plain background stands in.
  if (!has_last_state_) {
    return;
  }
  if (last_state_.slot_details && last_state_.slot_details->open) {
    draw_slot_details(*last_state_.slot_details, last_state_.enter_is_cross,
                      last_state_.status_message, last_state_.status_kind,
                      last_state_.restore_confirmation_pending,
                      last_state_.duplicate_backup_confirmation_pending);
  } else if (last_state_.saves && last_state_.visible_saves && last_state_.backup_rows) {
    draw_header(last_state_);
    draw_title_grid(last_state_);
    draw_backup_panel(last_state_);
    draw_footer(last_state_);
  } else {
    return;
  }
  vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(3, 7, 18, 200));
}

int Ui::details_max_scroll(const SlotDetailsState &state) const {
  if (state.metadata.slots.empty()) {
    return 0;
  }
  const std::size_t selected =
      std::min(state.selected_slot, state.metadata.slots.size() - 1);
  const std::string details = state.metadata.slots[selected].details.empty()
                                  ? "No description provided."
                                  : state.metadata.slots[selected].details;
  const std::vector<std::string> lines = wrap_text(kTextSizeSmall, details, kDetailsPaneWidth);
  return std::max(0, static_cast<int>(lines.size()) - kVisibleDetailLines);
}

void Ui::draw_slot_details(const SlotDetailsState &state, bool enter_is_cross,
                           const std::string &status_message, StatusKind status_kind,
                           bool restore_confirmation_pending,
                           bool duplicate_backup_confirmation_pending) {
  const bool confirmation_pending =
      restore_confirmation_pending || duplicate_backup_confirmation_pending;
  // This screen owns the full body so the backup list cannot distract from the currently
  // inspected snapshot. All values were loaded by App before this draw call.
  vita2d_draw_rectangle(0, 0, 960, 52, kColorHeader);
  vita2d_draw_line(0, 52, 960, 52, RGBA8(255, 255, 255, 20));
  draw_text(fonts_, 18, 34, kColorText, kTextSizeTitle, "Save Details");
  // The overview's cloud glyph repeats in the corner so the inspected snapshot's location (card,
  // Cloud, or both) stays visible; the live save shows neither flag and gets no glyph.
  const bool show_presence = state.snapshot_on_card || state.snapshot_in_cloud;
  const int context_right = show_presence ? 904 : 942;
  const std::string context = fit_text(
      kTextSizeSmall, state.game_title + "  /  " + display_backup_name(state.snapshot_name),
      680);
  draw_text(fonts_, context_right - measure_text(kTextSizeSmall, context.c_str()), 32,
            kColorMuted, kTextSizeSmall, context.c_str());
  if (show_presence) {
    draw_presence_glyph(914, 17, state.snapshot_on_card, state.snapshot_in_cloud);
  }

  // A full timestamp is meaningful here: saves in one game can span several years. Give the
  // summary pane enough room for YYYY-MM-DD HH:MM:SS instead of dropping the year to make it fit.
  constexpr int kDividerX = 308;
  constexpr int kLeftCardWidth = 272;
  constexpr int kRightX = 336;
  constexpr int kRightTextWidth = kDetailsPaneWidth;
  vita2d_draw_rectangle(0, 52, kDividerX, 456, kColorPanel);
  vita2d_draw_rectangle(kDividerX, 52, 960 - kDividerX, 456, RGBA8(15, 23, 42, 255));
  vita2d_draw_line(kDividerX, 52, kDividerX, 508, RGBA8(255, 255, 255, 20));

  std::string saved_display = "Unknown";
  if (state.metadata.saved_at.year > 0) {
    saved_display = format_save_datetime_spaced(state.metadata.saved_at);
  }
  // Summary card: the last save time, then whichever on-demand size rows apply. The card height
  // and the slot list below it shift down with the number of size rows so nothing overlaps.
  struct StatRow {
    const char *label;
    std::string value;
  };
  std::vector<StatRow> stats;
  if (state.save_bytes_known) {
    stats.push_back({"SAVE SIZE", format_bytes(state.save_bytes)});
  }
  if (state.archive_bytes_known) {
    stats.push_back({"BACKUP FILE SIZE", format_bytes(state.archive_bytes)});
  }

  const int card_right = 18 + kLeftCardWidth - 16;
  const int card_h = stats.empty() ? 68 : 76 + static_cast<int>(stats.size()) * 24;
  vita2d_draw_rectangle(18, 72, kLeftCardWidth, card_h, kColorPanelAlt);
  draw_text(fonts_, 34, 98, kColorMuted, kTextSizeTiny, "LAST SAVE TIME");
  draw_text(fonts_, 34, 124, kColorText, kTextSizeNormal, saved_display.c_str());
  if (!stats.empty()) {
    vita2d_draw_line(34, 140, card_right, 140, RGBA8(255, 255, 255, 18));
    for (std::size_t i = 0; i < stats.size(); ++i) {
      const int row_y = 162 + static_cast<int>(i) * 24;
      draw_text(fonts_, 34, row_y, kColorMuted, kTextSizeTiny, stats[i].label);
      draw_text(fonts_, card_right - measure_text(kTextSizeSmall, stats[i].value.c_str()), row_y,
                kColorText, kTextSizeSmall, stats[i].value.c_str());
    }
  }

  const int slots_heading_y = 72 + card_h + 24;
  const int slots_list_y = slots_heading_y + 12;
  draw_text(fonts_, 18, slots_heading_y, kColorMuted, kTextSizeTiny, "SLOTS");
  if (state.metadata.slots.empty()) {
    draw_text(fonts_, 34, slots_list_y + 22, kColorMuted, kTextSizeSmall, "No slot details");
  } else {
    // Fit as many slot rows as the space between the summary card and the footer allows. Rows are
    // 36 tall on a 42 pitch and the footer starts at 508, so the last top must satisfy
    // y + 36 <= 508, giving (514 - slots_list_y) / 42 rows; 7 is the no-stats parity cap.
    const std::size_t visible_slots =
        static_cast<std::size_t>(std::max(1, std::min(7, (514 - slots_list_y) / 42)));
    const std::size_t selected =
        std::min(state.selected_slot, state.metadata.slots.size() - 1);
    const std::size_t top = selected >= visible_slots ? selected - visible_slots + 1 : 0;
    const std::size_t count =
        std::min(visible_slots, state.metadata.slots.size() - top);
    for (std::size_t i = 0; i < count; ++i) {
      const SaveSlotMetadata &slot = state.metadata.slots[top + i];
      const bool focused = top + i == selected;
      const int y = slots_list_y + static_cast<int>(i) * 42;
      vita2d_draw_rectangle(18, y, kLeftCardWidth, 36,
                            focused ? kColorAccentSoft : RGBA8(255, 255, 255, 10));
      if (focused) {
        vita2d_draw_rectangle(18, y, 4, 36, kColorAccent);
      }
      char slot_label[16];
      std::snprintf(slot_label, sizeof(slot_label), "Slot %03u", slot.id);
      draw_text(fonts_, 32, y + 24, focused ? kColorText : kColorMuted, kTextSizeSmall,
                slot_label);
      const std::string slot_date = format_save_datetime_spaced(slot.modified_at);
      draw_datetime_tabular(fonts_, 276, y + 23, focused ? kColorText : kColorIdleDot,
                            kTextSizeTiny, slot_date);
    }
  }

  if (state.metadata.slots.empty()) {
    const std::string heading = state.unavailable_message.empty()
                                    ? "No slot details available"
                                    : state.unavailable_message;
    draw_text(fonts_, kRightX, 116, kColorText, kTextSizeTitle, heading.c_str());
    std::string explanation = state.warning_message;
    if (explanation.empty()) {
      explanation = state.metadata.source == SaveTimeSource::Filesystem
                        ? "The backup has a save time, but no readable save slot information."
                        : "This backup does not contain readable save slot information.";
    }
    const std::vector<std::string> lines =
        wrap_text(kTextSizeSmall, explanation, kRightTextWidth);
    for (std::size_t i = 0; i < lines.size() && i < 6; ++i) {
      draw_text(fonts_, kRightX, 156 + static_cast<int>(i) * 24, kColorMuted, kTextSizeSmall,
                lines[i].c_str());
    }
  } else {
    const std::size_t selected =
        std::min(state.selected_slot, state.metadata.slots.size() - 1);
    const SaveSlotMetadata &slot = state.metadata.slots[selected];
    // Echo the selected slot's number and time at the top of this pane (they are also in the left
    // list). When several slots share the same title/subtitle/details, this header is the only part
    // that visibly changes as you move between them, confirming the selection landed.
    char slot_label[16];
    std::snprintf(slot_label, sizeof(slot_label), "Slot %03u", slot.id);
    // Baseline 81 centers the digit ink exactly between the header rule (y=52) and the divider
    // below (y=96): 16px of air on both sides, measured from a hardware capture.
    draw_text(fonts_, kRightX, 81, kColorAccent, kTextSizeNormal, slot_label);
    const std::string slot_time = format_save_datetime_spaced(slot.modified_at);
    // Tabular so the date does not shift sideways as the selection moves between slots; same size
    // as the slot label so the pair reads as one headline (a smaller date on the same baseline
    // started its caps lower and looked misaligned).
    draw_datetime_tabular(fonts_, 926, 81, kColorText, kTextSizeNormal, slot_time);
    vita2d_draw_line(kRightX, 96, 926, 96, RGBA8(255, 255, 255, 20));

    draw_text(fonts_, kRightX, 128, kColorMuted, kTextSizeTiny, "TITLE");
    draw_text(fonts_, kRightX, 154, kColorText, kTextSizeNormal,
              fit_text(kTextSizeNormal, slot.title.empty() ? "Untitled" : slot.title,
                       kRightTextWidth)
                  .c_str());
    draw_text(fonts_, kRightX, 196, kColorMuted, kTextSizeTiny, "SUBTITLE");
    draw_text(fonts_, kRightX, 222, kColorText, kTextSizeSmall,
              fit_text(kTextSizeSmall, slot.subtitle.empty() ? "—" : slot.subtitle,
                       kRightTextWidth)
                  .c_str());
    draw_text(fonts_, kRightX, 262, kColorMuted, kTextSizeTiny, "DETAILS");

    const std::string details =
        slot.details.empty() ? "No description provided." : slot.details;
    const std::vector<std::string> lines =
        wrap_text(kTextSizeSmall, details, kRightTextWidth);
    const int max_scroll = std::max(0, static_cast<int>(lines.size()) - kVisibleDetailLines);
    const int scroll = std::min(std::max(0, state.details_scroll), max_scroll);
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(kRightX, 270, 926, 502);
    for (int i = 0; i < kVisibleDetailLines && scroll + i < static_cast<int>(lines.size()); ++i) {
      draw_text(fonts_, kRightX, 286 + i * 22, kColorText, kTextSizeSmall,
                lines[static_cast<std::size_t>(scroll + i)].c_str());
    }
    vita2d_disable_clipping();
    if (max_scroll > 0) {
      constexpr int kTrackY = 274;
      constexpr int kTrackH = 224;
      vita2d_draw_rectangle(942, kTrackY, 3, kTrackH, RGBA8(255, 255, 255, 24));
      const int thumb_h = std::max(24, kTrackH * kVisibleDetailLines /
                                          static_cast<int>(lines.size()));
      const int thumb_y = kTrackY + (kTrackH - thumb_h) * scroll / max_scroll;
      vita2d_draw_rectangle(942, thumb_y, 3, thumb_h, kColorAccent);
    }
  }

  vita2d_draw_rectangle(0, 508, 960, 36, RGBA8(3, 7, 18, 230));
  const ButtonSymbol cancel = enter_is_cross ? ButtonSymbol::Circle : ButtonSymbol::Cross;
  const ButtonSymbol primary = enter_is_cross ? ButtonSymbol::Cross : ButtonSymbol::Circle;
  // The overview's per-snapshot actions carry over, with the same button meanings: Select
  // transfers to the side the snapshot is missing from, Square labels, the action button backs
  // up the live save or restores the inspected snapshot.
  const bool is_snapshot = state.snapshot_on_card || state.snapshot_in_cloud;
  std::vector<HintSpec> hints;
  if (confirmation_pending) {
    // A pending prompt owns the footer: only the two buttons that answer it are offered.
    hints.push_back({primary, restore_confirmation_pending ? "Confirm restore"
                                                           : "Confirm backup"});
    hints.push_back({cancel, "Cancel"});
  } else {
    if (details_max_scroll(state) > 0) {
      hints.push_back({ButtonSymbol::Label, "R-stick  Scroll"});
    }
    if (is_snapshot) {
      hints.push_back({ButtonSymbol::Square, "Label"});
      if (!state.snapshot_on_card) {
        hints.push_back({ButtonSymbol::Select, "Download"});
      } else if (!state.snapshot_in_cloud) {
        hints.push_back({ButtonSymbol::Select, "Upload"});
      }
    }
    hints.push_back({primary, is_snapshot ? "Restore" : "Backup"});
    hints.push_back({cancel, "Back"});
  }
  const int hints_left =
      draw_hints_right_aligned(fonts_, hints.data(), static_cast<int>(hints.size()));

  // The overview's status line, mirrored here so action feedback and confirmation prompts are
  // visible without leaving the screen. It gets exactly the room the hints leave over.
  if (!status_message.empty()) {
    unsigned int color = kColorMuted;
    if (confirmation_pending) {
      color = kColorAccent;
    } else if (status_kind == StatusKind::Success) {
      color = kColorSuccess;
    } else if (status_kind == StatusKind::Error) {
      color = kColorError;
    }
    draw_text(fonts_, 18, 531, color, kTextSizeSmall,
              fit_text(kTextSizeSmall, status_message, std::max(120, hints_left - 48)).c_str());
  }
}

void Ui::draw_header(const UiState &state) {
  vita2d_draw_rectangle(0, 0, 960, 52, kColorHeader);
  vita2d_draw_line(0, 52, 960, 52, RGBA8(255, 255, 255, 20));

  draw_text(fonts_, 18, 34, kColorText, kTextSizeTitle, "Save Keeper");

  // "connected" means a working sign-in; without internet the account is only configured, so the
  // header says offline instead of pretending Drive is reachable.
  const bool offline = state.google_connected && !state.network_connected;
  const char *drive_status = state.google_auth_pending ? "Connecting to Google..."
                             : !state.google_connected ? "Google Drive not connected"
                             : offline                 ? "Google Drive offline"
                             : state.drive_synced      ? "Google Drive synced"
                                                       : "Google Drive connected";
  const unsigned int dot_color = offline                     ? kColorPendingDot
                                 : state.google_connected    ? kColorSuccess
                                 : state.google_auth_pending ? kColorPendingDot
                                                             : kColorIdleDot;
  const int status_x = 960 - 18 - measure_text(kTextSizeSmall, drive_status);
  vita2d_draw_fill_circle(status_x - 14, 26, 5, dot_color);
  draw_text(fonts_, status_x, 32, kColorMuted, kTextSizeSmall, drive_status);

  // The connect gesture lives here next to the state it fixes, not in the footer: "not
  // connected" and "hold Triangle to connect" belong in one glance.
  if (!state.google_connected && !state.google_auth_pending) {
    const char *hold_text = "hold";
    const char *connect_text = "to connect";
    const int hold_w = measure_text(kTextSizeSmall, hold_text);
    const int connect_w = measure_text(kTextSizeSmall, connect_text);
    const int glyph_w = 16;
    const int cue_w = hold_w + 6 + glyph_w + 6 + connect_w;
    const int cue_x = status_x - 19 - 24 - cue_w;
    draw_text(fonts_, cue_x, 32, kColorMuted, kTextSizeSmall, hold_text);
    draw_button_shape(cue_x + hold_w + 6, 17, ButtonSymbol::Triangle, kColorMuted);
    draw_text(fonts_, cue_x + hold_w + 6 + glyph_w + 6, 32, kColorMuted, kTextSizeSmall,
              connect_text);
  }
}

void Ui::draw_title_grid(const UiState &state) {
  const std::vector<SaveRecord> &saves = *state.saves;
  const std::vector<std::size_t> &visible = *state.visible_saves;
  vita2d_draw_rectangle(0, 52, 504, 456, kColorPanel);
  vita2d_draw_line(504, 52, 504, 508, RGBA8(255, 255, 255, 20));

  // Category tabs: L/R cycles Vita / Homebrew / PSP. Tabs with zero saves stay dimmed and are
  // skipped by the input handler.
  int tab_x = 40;
  for (int i = 0; i < kSaveCategoryCount; ++i) {
    const SaveCategory category = static_cast<SaveCategory>(i);
    const std::string label = std::string(save_category_label(category)) + " " +
                              std::to_string(state.category_counts[static_cast<std::size_t>(i)]);
    const bool active = category == state.active_category;
    const bool empty = state.category_counts[static_cast<std::size_t>(i)] == 0;
    const unsigned int color = active ? kColorText : empty ? kColorIdleDot : kColorMuted;
    draw_text(fonts_, tab_x, 84, color, kTextSizeSmall, label.c_str());
    const int width = measure_text(kTextSizeSmall, label.c_str());
    if (active) {
      vita2d_draw_rectangle(tab_x, 90, width, 3, kColorAccent);
    }
    tab_x += width + 20;
  }
  // Small L/R chips around the tab row show which buttons switch groups.
  draw_text(fonts_, 16, 84, kColorIdleDot, kTextSizeTiny, "L");
  draw_text(fonts_, tab_x, 84, kColorIdleDot, kTextSizeTiny, "R");

  constexpr int kColumns = kSaveGridColumns;
  constexpr int kRows = 4;
  constexpr int kTileSize = 88;
  constexpr int kGapX = 8;
  constexpr int kGapY = 12;
  constexpr int kStartX = 16;
  constexpr int kStartY = 104;
  const std::size_t selected = visible.empty() ? 0 : state.selected_save % visible.size();
  title_top_row_ = grid_window_top_row(title_top_row_, selected, visible.size(), kColumns, kRows);
  const std::size_t first_index = title_top_row_ * kColumns;

  if (!visible.empty()) {
    const std::string count =
        std::to_string(selected + 1) + "/" + std::to_string(visible.size());
    draw_text(fonts_, 504 - 16 - measure_text(kTextSizeSmall, count.c_str()), 84,
              kColorMuted, kTextSizeSmall, count.c_str());
  }

  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kColumns; ++col) {
      const std::size_t index = first_index + static_cast<std::size_t>(row * kColumns + col);
      const int x = kStartX + col * (kTileSize + kGapX);
      const int y = kStartY + row * (kTileSize + kGapY);
      if (index >= visible.size()) {
        vita2d_draw_rectangle(x, y, kTileSize, kTileSize, RGBA8(255, 255, 255, 8));
        continue;
      }

      const bool is_selected = index == selected;
      const SaveRecord &save = saves[visible[index]];
      vita2d_draw_rectangle(x - 3, y - 3, kTileSize + 6, kTileSize + 6,
                            is_selected ? kColorAccent : RGBA8(255, 255, 255, 14));
      vita2d_draw_rectangle(x, y, kTileSize, kTileSize,
                            is_selected ? RGBA8(25, 45, 64, 255) : kColorPanelAlt);

      const int pad = is_selected ? 4 : 8;
      const int icon_size = kTileSize - pad * 2;
      vita2d_texture *texture = load_icon_texture(save.icon_path);
      if (texture) {
        vita2d_draw_rectangle(x + pad, y + pad, icon_size, icon_size, RGBA8(0, 0, 0, 120));
        draw_texture_fit(texture, x + pad, y + pad, icon_size);
      } else {
        draw_placeholder_icon(fonts_, save, x + pad, y + pad, icon_size,
                              is_selected);
      }
    }
  }

  if (visible.empty()) {
    // Selected-save details live at the top of the right pane; the grid area only needs the
    // empty states.
    if (saves.empty()) {
      draw_text(fonts_, 120, 280, kColorText, kTextSizeNormal, "No saves found");
      draw_text(fonts_, 120, 306, kColorMuted, kTextSizeSmall,
                "Checked Vita, game card, and PSP roots");
    } else {
      draw_text(fonts_, 120, 280, kColorText, kTextSizeNormal,
                "No saves in this group");
      draw_text(fonts_, 120, 306, kColorMuted, kTextSizeSmall,
                "L / R switches groups");
    }
  }
}

void Ui::draw_backup_panel(const UiState &state) {
  vita2d_draw_rectangle(504, 52, 456, 456, RGBA8(15, 23, 42, 255));

  if (state.google_auth_pending && !state.google_user_code.empty()) {
    draw_google_auth_panel(state);
    draw_status_line(state);
    return;
  }

  const SaveRecord *save = selected_visible_record(state);
  if (!save) {
    draw_text(fonts_, 528, 96, kColorMuted, kTextSizeSmall,
              "Install or create saves, then reopen Save Keeper.");
    draw_status_line(state);
    return;
  }

  // Selected-save details live here, in one place, above its backup menu. Truncation is by
  // measured pixel width so long titles use the full pane.
  const std::string title = fit_text(kTextSizeNormal, save->display_name, 408);
  const char *platform_text = classify_save(*save) == SaveCategory::Homebrew
                                  ? "Homebrew"
                                  : platform_label(save->platform);
  const std::string details = fit_text(
      kTextSizeSmall, title_id_label(*save) + "  |  " + platform_text + " save", 408);
  // Baseline 84 matches the tab row in the left pane so the top lines read as one row.
  draw_text(fonts_, 528, 84, kColorText, kTextSizeNormal, title.c_str());
  draw_text(fonts_, 528, 110, kColorMuted, kTextSizeSmall, details.c_str());

  if (!state.backup_rows) {
    draw_status_line(state);
    return;
  }

  constexpr std::size_t kVisibleEntries = 7;
  // Row indices match the App: 0 is the always-selectable "New Backup" entry. The window only
  // scrolls when the selection reaches its edge, like the save grid.
  const std::vector<BackupRow> &all_rows = *state.backup_rows;
  const std::size_t selected_entry = state.selected_backup;
  backup_top_row_ = grid_window_top_row(backup_top_row_, selected_entry, all_rows.size(), 1,
                                        kVisibleEntries);
  const std::size_t backup_window = backup_top_row_;
  const std::size_t visible_count =
      std::min(kVisibleEntries, all_rows.size() - std::min(backup_window, all_rows.size()));

  int y = 136;
  for (std::size_t i = 0; i < visible_count; ++i) {
    const BackupRow &row = all_rows[i + backup_window];
    const bool selected = (i + backup_window) == selected_entry;
    const unsigned int bg = selected ? kColorAccentSoft : RGBA8(255, 255, 255, 18);
    vita2d_draw_rectangle(528, y, 408, 36, bg);
    if (selected) {
      vita2d_draw_rectangle(528, y, 4, 36, kColorAccent);
    }

    // Each snapshot marks its state in one fixed right-aligned slot: synced (card + Drive),
    // Drive-only (amber down arrow), or card-only (slate up arrow - Select uploads it). Full
    // color even when unselected: the hue is the signal, so dimming it would cost meaning.
    int max_text_width = 386;
    if (row.new_backup) {
      // The focused save's time is shown here. Encrypted saves resolve it lazily through a mount a
      // moment after the selection settles; a circle spinner stands in until then, instead of
      // flashing a misleading "unknown". A resolved save with no trustworthy time reads "unknown".
      const unsigned int time_color = selected ? kColorAccent : kColorIdleDot;
      if (save->save_time_requires_mount) {
        static const char *const kPrefix = "Last save:";
        draw_text(fonts_, 898 - measure_text(kTextSizeTiny, kPrefix), y + 23, time_color,
                  kTextSizeTiny, kPrefix);
        draw_circle_spinner(912.0f, static_cast<float>(y) + 17.0f, frame_counter_, time_color);
      } else {
        const std::string current_save_time =
            save->save_time_known ? "Last save: " + format_save_datetime_spaced(save->saved_at)
                                  : "Last save: unknown";
        draw_text(fonts_, 922 - measure_text(kTextSizeTiny, current_save_time.c_str()), y + 23,
                  time_color, kTextSizeTiny, current_save_time.c_str());
      }
      max_text_width = 150;
    } else {
      max_text_width = 340;
      draw_presence_glyph(902, y + 9, row.has_local(), row.has_remote());
    }
    const std::string display = row.display_name();
    const int full_width = measure_text(kTextSizeSmall, display.c_str());
    if (selected && full_width > max_text_width) {
      // Focused row with a name too long for the row: marquee the full text inside a clip window
      // instead of ellipsizing, pausing at each end so both ends stay readable. The scroll
      // restarts whenever the selection moves to a different row.
      if (marquee_entry_ != i + backup_window) {
        marquee_entry_ = i + backup_window;
        marquee_frame_ = 0;
      } else {
        ++marquee_frame_;
      }
      const unsigned int travel = static_cast<unsigned int>(full_width - max_text_width);
      constexpr unsigned int kMarqueePauseFrames = 45;
      const unsigned int cycle = 2 * (kMarqueePauseFrames + travel);
      const unsigned int t = marquee_frame_ % cycle;
      unsigned int offset;
      if (t < kMarqueePauseFrames) {
        offset = 0;
      } else if (t < kMarqueePauseFrames + travel) {
        offset = t - kMarqueePauseFrames;
      } else if (t < 2 * kMarqueePauseFrames + travel) {
        offset = travel;
      } else {
        offset = travel - (t - 2 * kMarqueePauseFrames - travel);
      }
      vita2d_enable_clipping();
      vita2d_set_clip_rectangle(542, y, 542 + max_text_width, y + 36);
      draw_text(fonts_, 542 - static_cast<int>(offset), y + 24, kColorText, kTextSizeSmall,
                display.c_str());
      vita2d_disable_clipping();
    } else {
      const std::string label = fit_text(kTextSizeSmall, display, max_text_width);
      draw_text(fonts_, 542, y + 24, selected ? kColorText : kColorMuted,
                kTextSizeSmall, label.c_str());
    }
    y += 42;
  }

  // Chevrons say "more above/below" without spending a text row on them; both sit 8px from the
  // list edges (list spans y 136..424 when full).
  if (backup_window > 0) {
    vita2d_draw_line(714, 128, 720, 122, kColorMuted);
    vita2d_draw_line(720, 122, 726, 128, kColorMuted);
  }
  if (all_rows.size() > backup_window + visible_count) {
    vita2d_draw_line(714, 432, 720, 438, kColorMuted);
    vita2d_draw_line(720, 438, 726, 432, kColorMuted);
  }

  if (all_rows.size() > 1) {
    draw_rstick_hint(930, 470);
  }

  draw_status_line(state);
}

void Ui::draw_rstick_hint(int cx, int cy) {
  // Right-stick pictogram: a stick cap marked R with up/down chevrons.
  vita2d_draw_fill_circle(cx, cy, 9, RGBA8(255, 255, 255, 26));
  draw_text(fonts_, cx - 4, cy + 5, kColorIdleDot, kTextSizeTiny, "R");
  vita2d_draw_line(cx - 4, cy - 14, cx, cy - 18, kColorIdleDot);
  vita2d_draw_line(cx, cy - 18, cx + 4, cy - 14, kColorIdleDot);
  vita2d_draw_line(cx - 4, cy + 14, cx, cy + 18, kColorIdleDot);
  vita2d_draw_line(cx, cy + 18, cx + 4, cy + 14, kColorIdleDot);
}

void Ui::draw_google_auth_panel(const UiState &state) {
  draw_text(fonts_, 528, 90, kColorText, kTextSizeTitle, "Connect Google Drive");

  // Short step lines: the QR block occupies the pane's right edge from x=804.
  draw_text(fonts_, 528, 138, kColorText, kTextSizeNormal,
            "1  Scan the QR code");
  draw_text(fonts_, 528, 168, kColorText, kTextSizeNormal,
            "2  Approve Save Keeper");
  draw_text(fonts_, 528, 198, kColorText, kTextSizeNormal,
            "3  Finishes by itself");

  const std::string url = "Or enter the code at " + display_url(state.google_verification_url);
  draw_text(fonts_, 528, 308, kColorMuted, kTextSizeTiny,
            truncate_label(url, 40).c_str());
  draw_text(fonts_, 528, 356, kColorAccent, kTextSizeCode,
            state.google_user_code.c_str());

  // The QR code carries the verification URL with the user code pre-filled, so scanning skips the
  // typing step entirely.
  const std::string qr_url = build_device_verification_qr_url(state.google_verification_url,
                                                              state.google_user_code);
  draw_qr_code(qr_url, 770, 104, 5);

  // A lightweight pulse so the wait feels alive: one dot per half second, cycling.
  const int dot_count = 1 + static_cast<int>((frame_counter_ / 30U) % 3U);
  std::string waiting = "Waiting for approval";
  for (int i = 0; i < dot_count; ++i) {
    waiting.push_back('.');
  }
  draw_text(fonts_, 528, 416, kColorMuted, kTextSizeNormal, waiting.c_str());

  const std::string validity =
      "Code valid for " + format_minutes_seconds(state.auth_seconds_left);
  draw_text(fonts_, 528, 444, kColorMuted, kTextSizeTiny, validity.c_str());
}

void Ui::draw_status_line(const UiState &state) {
  if (state.status_message.empty()) {
    return;
  }
  const std::string status = fit_text(kTextSizeSmall, state.status_message, 380);
  unsigned int color = kColorMuted;
  if (state.restore_confirmation_pending || state.delete_confirmation_pending ||
      state.delete_scope_prompt_pending || state.sync_all_confirmation_pending ||
      state.duplicate_backup_confirmation_pending) {
    color = kColorAccent;
  } else if (!state.status_message.empty()) {
    switch (state.status_kind) {
    case StatusKind::Success:
      color = kColorSuccess;
      break;
    case StatusKind::Error:
      color = kColorError;
      break;
    case StatusKind::Info:
      color = kColorMuted;
      break;
    }
  }
  // Baseline chosen so the text centers on the R-stick pictogram (cy = 470) next to it.
  draw_text(fonts_, 528, 476, color, kTextSizeSmall, status.c_str());

  // Hold gauge (Select = batch, Square = label, Triangle = Google action): fills toward the
  // trigger right under the hint text, so the gesture announces itself before anything runs.
  if (state.hold_gauge_fraction > 0.0f) {
    const float fraction = std::min(1.0f, state.hold_gauge_fraction);
    vita2d_draw_rectangle(528, 482, 380, 3, RGBA8(255, 255, 255, 24));
    vita2d_draw_rectangle(528, 482, static_cast<int>(380.0f * fraction), 3, kColorAccent);
  }
}

void Ui::draw_footer(const UiState &state) {
  vita2d_draw_rectangle(0, 508, 960, 36, RGBA8(3, 7, 18, 230));

  const ButtonSymbol confirm = state.enter_is_cross ? ButtonSymbol::Cross : ButtonSymbol::Circle;
  const ButtonSymbol cancel = state.enter_is_cross ? ButtonSymbol::Circle : ButtonSymbol::Cross;

  // The footer follows the current mode so a cancel hint only appears when there is actually
  // something to cancel. Hints are right-aligned with the primary action at the right edge.
  if (state.google_setup_prompt != GoogleSetupPrompt::None) {
    const HintSpec hints[] = {{cancel, "Close"}};
    draw_hints_right_aligned(fonts_, hints, 1);
    return;
  }
  if (state.google_auth_pending) {
    const HintSpec hints[] = {{cancel, "Cancel Sign-In"}, {ButtonSymbol::Triangle, "Check Now"}};
    draw_hints_right_aligned(fonts_, hints, 2);
    return;
  }
  if (state.restore_confirmation_pending) {
    const HintSpec hints[] = {{cancel, "Cancel"}, {confirm, "Confirm Restore"}};
    draw_hints_right_aligned(fonts_, hints, 2);
    return;
  }
  if (state.delete_scope_prompt_pending) {
    // One "Delete:" label introduces the three scope buttons, so each button just names its
    // target instead of repeating the verb.
    const HintSpec hints[] = {{cancel, "Cancel"},
                              {ButtonSymbol::Label, "Delete:"},
                              {ButtonSymbol::Square, "Local"},
                              {ButtonSymbol::Triangle, "Cloud"},
                              {ButtonSymbol::Start, "Both"}};
    draw_hints_right_aligned(fonts_, hints, 5);
    return;
  }
  if (state.delete_confirmation_pending) {
    const HintSpec hints[] = {{cancel, "Cancel"}, {ButtonSymbol::Start, "Confirm Delete"}};
    draw_hints_right_aligned(fonts_, hints, 2);
    return;
  }
  if (state.sync_all_confirmation_pending) {
    const HintSpec hints[] = {{cancel, "Cancel"},
                              {confirm, state.sync_all_will_upload ? "Confirm Backup & Upload All"
                                                                   : "Confirm Backup All"}};
    draw_hints_right_aligned(fonts_, hints, 2);
    return;
  }
  if (state.duplicate_backup_confirmation_pending) {
    const HintSpec hints[] = {{cancel, "Cancel"}, {confirm, "Create New Backup Anyway"}};
    draw_hints_right_aligned(fonts_, hints, 2);
    return;
  }

  // One context action: create a backup on the "New Backup" entry, restore on a backup entry.
  const std::string sort_label =
      std::string("Sort: ") + save_sort_mode_label(state.sort_mode);
  const BackupRow *focused_row = nullptr;
  if (state.backup_rows && state.selected_backup > 0 &&
      state.selected_backup < state.backup_rows->size()) {
    focused_row = &(*state.backup_rows)[state.selected_backup];
  }
  std::vector<HintSpec> hints;
  hints.reserve(5);
  // Square taps to sort and holds to label. The sort-mode readout always stays visible; the
  // label gesture appears as a dim qualifier only on backup rows, where it applies.
  hints.push_back({ButtonSymbol::Square, sort_label.c_str(), nullptr,
                   focused_row ? "(hold: Label)" : nullptr});
  // The connect cue lives in the header next to "not connected"; the footer only notes the
  // refresh hold once a connection exists.
  hints.push_back({ButtonSymbol::Triangle, "Details", nullptr,
                   state.google_connected ? "(hold: Refresh)" : nullptr});
  hints.push_back({ButtonSymbol::Start, "Delete", nullptr, nullptr});
  // Select moves the focused snapshot across: Upload from a card-only row, Download to the card
  // from a Drive-only row. A synced row has no tap action left, so the slot disappears rather
  // than advertise a no-op; on the New Backup entry it names the hold gesture instead, with the
  // "(hold)" qualifier dim and small so it reads as a gesture note, not the action name.
  if (!focused_row) {
    hints.push_back({ButtonSymbol::Select, "Backup & Upload All", "(hold)", nullptr});
  } else if (!focused_row->has_local()) {
    hints.push_back({ButtonSymbol::Select, "Download", nullptr, nullptr});
  } else if (!focused_row->has_remote()) {
    hints.push_back({ButtonSymbol::Select, "Upload", nullptr, nullptr});
  }
  hints.push_back({confirm, state.selected_backup == 0 ? "Backup" : "Restore", nullptr, nullptr});
  draw_hints_right_aligned(fonts_, hints.data(), static_cast<int>(hints.size()));
}

void Ui::set_batch_progress(std::string action, std::string game, std::size_t done_games,
                            std::size_t total_games, bool cancel_is_circle) {
  batch_action_ = std::move(action);
  batch_game_ = std::move(game);
  batch_done_ = done_games;
  batch_total_ = total_games;
  batch_cancel_is_circle_ = cancel_is_circle;
  batch_active_ = batch_total_ > 0;
}

void Ui::clear_batch_progress() {
  batch_active_ = false;
  batch_action_.clear();
  batch_game_.clear();
}

void Ui::draw_google_setup_prompt(const UiState &state) {
  vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(3, 7, 18, 200));

  constexpr int kBoxX = 116;
  constexpr int kBoxY = 104;
  constexpr int kBoxW = 728;
  constexpr int kBoxH = 328;
  vita2d_draw_rectangle(kBoxX, kBoxY, kBoxW, kBoxH, kColorPanelAlt);
  vita2d_draw_rectangle(kBoxX, kBoxY, kBoxW, 4, kColorAccent);

  draw_text(fonts_, kBoxX + 28, kBoxY + 48, kColorText, kTextSizeTitle,
            "Google Drive setup needed");

  const bool missing = state.google_setup_prompt == GoogleSetupPrompt::MissingFile;
  const int text_x = kBoxX + 28;
  int y = kBoxY + 96;
  if (missing) {
    // Only Roboto-Regular is bundled, so the app name gets its bold from a 1px double draw.
    draw_text(fonts_, text_x, y, kColorText, kTextSizeSmall, "Save Keeper");
    draw_text(fonts_, text_x + 1, y, kColorText, kTextSizeSmall, "Save Keeper");
    draw_text(fonts_, text_x + measure_text(kTextSizeSmall, "Save Keeper") + 1, y, kColorText,
              kTextSizeSmall, " syncs backups to your own Google Drive.");
    y += 28;
    draw_text(fonts_, text_x, y, kColorText, kTextSizeSmall,
              "That needs a one-time setup - free, about ten minutes.");
    y += 28 + 16;
    draw_text(fonts_, text_x, y, kColorText, kTextSizeSmall, "No credentials file was found at:");
    y += 28;
  } else if (state.google_setup_prompt == GoogleSetupPrompt::InvalidFile) {
    draw_text(fonts_, text_x, y, kColorText, kTextSizeSmall,
              "The credentials file could not be used - it needs");
    y += 28;
    draw_text(fonts_, text_x, y, kColorText, kTextSizeSmall,
              "client_id and client_secret. Redo the last guide");
    y += 28;
    draw_text(fonts_, text_x, y, kColorText, kTextSizeSmall, "step and replace the file at:");
    y += 28;
  } else {
    draw_text(fonts_, text_x, y, kColorText, kTextSizeSmall,
              "Google did not accept these credentials.");
    y += 28;
    draw_text(fonts_, text_x, y, kColorText, kTextSizeSmall,
              "The OAuth client may have been deleted or changed.");
    y += 28;
    draw_text(fonts_, text_x, y, kColorText, kTextSizeSmall,
              "Redo the guide and replace the file at:");
    y += 28;
  }
  draw_text(fonts_, text_x, y, kColorMuted, kTextSizeSmall,
            "ux0:data/save-keeper/google-client.json");
  y += 44;
  draw_text(fonts_, text_x, y, kColorText, kTextSizeSmall, "Scan the QR code to open the ");
  const int scan_w = measure_text(kTextSizeSmall, "Scan the QR code to open the ");
  draw_text(fonts_, text_x + scan_w, y, kColorText, kTextSizeSmall, "Google Drive Setup Guide.");
  draw_text(fonts_, text_x + scan_w + 1, y, kColorText, kTextSizeSmall,
            "Google Drive Setup Guide.");

  // 86 chars at ECC LOW is a 37-module code; 5px modules plus the quiet zone make 205px.
  constexpr int kQrSize = 205;
  constexpr int kQrX = kBoxX + kBoxW - 28 - kQrSize;
  constexpr int kQrY = kBoxY + 52;
  draw_qr_code(kGoogleSetupGuideUrl, kQrX, kQrY, 5);
  // The QR's destination spelled out, hanging from the code's bottom-right corner.
  const char *url_display =
      "github.com/falkenhawk/vita-save-keeper/blob/master/docs/google-drive-setup.md";
  const int url_w = measure_text(kTextSizeTiny, url_display);
  draw_text(fonts_, kBoxX + kBoxW - 28 - url_w, kQrY + kQrSize + 24, kColorMuted, kTextSizeTiny,
            url_display);
}

void Ui::draw_busy(const std::string &label, long long done, long long total,
                   const char *context_above, const char *cancel_hint) {
  ++frame_counter_;
  vita2d_start_drawing();
  vita2d_clear_screen();
  draw_modal_backdrop();

  constexpr int kBoxX = 280;
  constexpr int kBoxY = 216;
  constexpr int kBoxW = 400;
  constexpr int kBoxH = 112;
  vita2d_draw_rectangle(kBoxX, kBoxY, kBoxW, kBoxH, kColorPanelAlt);
  vita2d_draw_rectangle(kBoxX, kBoxY, kBoxW, 4, kColorAccent);

  if (context_above != nullptr) {
    const int w = text_width(fonts_, kTextSizeSmall, context_above);
    draw_text(fonts_, kBoxX + kBoxW / 2 - w / 2, kBoxY - 12, kColorMuted, kTextSizeSmall,
              context_above);
  }

  // The title and the bar track overall games progress in a batch. The in-flight transfer
  // percent rides on the title line next to "Uploading ..." - it belongs to that one file - and
  // only while bytes are actually moving. Backups report no byte progress, so nothing per-file
  // shows while zipping, which is why a fast per-game backup no longer flashes a stray 100%.
  const bool batch_transfer = batch_active_ && total > 0;
  char transfer_pct[16] = {0};
  int title_max_w = kBoxW - 48;
  if (batch_transfer) {
    float f = std::max(0.0f, std::min(1.0f, static_cast<float>(done) / static_cast<float>(total)));
    std::snprintf(transfer_pct, sizeof(transfer_pct), "%d%%", static_cast<int>(f * 100.0f));
    title_max_w -= text_width(fonts_, kTextSizeSmall, transfer_pct) + 12;
  }
  // In a batch, ellipsize only the game title so the "(N/M)" counter is never cut; a single
  // operation just fits its whole label. Game titles are of arbitrary length either way.
  std::string fitted_label;
  if (batch_active_) {
    const std::string counter =
        " (" + std::to_string(batch_done_ + 1) + "/" + std::to_string(batch_total_) + ")";
    fitted_label = fit_quoted_name(batch_action_ + " ", batch_game_, counter, kTextSizeNormal,
                                   title_max_w, /*quote=*/false);
  } else {
    fitted_label = fit_text(kTextSizeNormal, label, title_max_w);
  }
  // The title block sits so the cap top clears the accent strip by the same 20px the baseline
  // keeps above the bar (a size-18 cap is ~13px tall); the old +42/+62 layout read heavier on
  // top. The whole stack below shifts with it, so the slack goes to the box's bottom padding.
  draw_text(fonts_, kBoxX + 24, kBoxY + 37, kColorText, kTextSizeNormal, fitted_label.c_str());
  if (batch_transfer) {
    const int pw = text_width(fonts_, kTextSizeSmall, transfer_pct);
    draw_text(fonts_, kBoxX + kBoxW - 24 - pw, kBoxY + 37, kColorAccent, kTextSizeSmall,
              transfer_pct);
  }

  const int bar_x = kBoxX + 24;
  const int bar_y = kBoxY + 57;
  const int bar_w = kBoxW - 48;
  const int bar_h = 12;
  vita2d_draw_rectangle(bar_x, bar_y, bar_w, bar_h, RGBA8(255, 255, 255, 24));

  const long long bar_done = batch_active_ ? static_cast<long long>(batch_done_) : done;
  const long long bar_total = batch_active_ ? static_cast<long long>(batch_total_) : total;
  if (bar_total > 0) {
    float fraction = static_cast<float>(bar_done) / static_cast<float>(bar_total);
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    vita2d_draw_rectangle(bar_x, bar_y, static_cast<int>(bar_w * fraction), bar_h, kColorAccent);
  } else {
    // Unknown size: sweep a segment across the bar so the app visibly keeps working.
    const int segment = bar_w / 4;
    const int travel = bar_w - segment;
    const int offset = static_cast<int>((frame_counter_ * 8U) % static_cast<unsigned int>(travel * 2));
    const int start = offset <= travel ? offset : travel * 2 - offset;
    vita2d_draw_rectangle(bar_x + start, bar_y, segment, bar_h, kColorAccent);
  }

  // Under the bar: the overall figure the bar represents. In a batch that is games completed of
  // the total (matching the bar); for a single operation it is that operation's own transfer.
  char overall_pct[16] = {0};
  bool show_overall = false;
  if (batch_active_ && batch_total_ > 0) {
    float f = std::max(0.0f, std::min(1.0f, static_cast<float>(batch_done_) /
                                                static_cast<float>(batch_total_)));
    std::snprintf(overall_pct, sizeof(overall_pct), "%d%%", static_cast<int>(f * 100.0f));
    show_overall = true;
  } else if (!batch_active_ && total > 0) {
    float f = std::max(0.0f, std::min(1.0f, static_cast<float>(done) / static_cast<float>(total)));
    std::snprintf(overall_pct, sizeof(overall_pct), "%d%%", static_cast<int>(f * 100.0f));
    show_overall = true;
  }
  if (show_overall) {
    draw_text(fonts_, bar_x, kBoxY + 93, kColorMuted, kTextSizeSmall, overall_pct);
  }

  if (batch_active_) {
    // "hold (cancel) to cancel" hint, right-aligned on the percent line.
    const char *hold_text = "hold";
    const char *cancel_text = "to cancel";
    const int hold_w = text_width(fonts_, kTextSizeSmall, hold_text);
    const int cancel_w = text_width(fonts_, kTextSizeSmall, cancel_text);
    int x = kBoxX + kBoxW - 24 - (hold_w + 6 + 22 + cancel_w);
    draw_text(fonts_, x, kBoxY + 93, kColorMuted, kTextSizeSmall, hold_text);
    x += hold_w + 6;
    draw_button_shape(x, kBoxY + 79,
                      batch_cancel_is_circle_ ? ButtonSymbol::Circle : ButtonSymbol::Cross,
                      kColorMuted);
    x += 22;
    draw_text(fonts_, x, kBoxY + 93, kColorMuted, kTextSizeSmall, cancel_text);
  }

  if (cancel_hint != nullptr) {
    const int text_w = text_width(fonts_, kTextSizeSmall, cancel_hint);
    int x = kBoxX + kBoxW / 2 - (22 + text_w) / 2;
    draw_button_shape(x, kBoxY + kBoxH + 8, ButtonSymbol::Square, kColorMuted);
    x += 22;
    draw_text(fonts_, x, kBoxY + kBoxH + 22, kColorMuted, kTextSizeSmall, cancel_hint);
  }

  vita2d_end_drawing();
  vita2d_swap_buffers();
}

TextInputResult Ui::prompt_text_input(const char *title, const std::string &initial_text,
                                      std::size_t max_length, std::string *out_text) {
  // The dialog reads these buffers for its whole lifetime; they stay alive because this call
  // blocks until the dialog is closed.
  SceWChar16 title_utf16[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1] = {};
  SceWChar16 initial_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1] = {};
  SceWChar16 input_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1] = {};
  utf8_to_utf16(title, title_utf16, SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1);
  utf8_to_utf16(initial_text, initial_utf16, SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1);

  SceImeDialogParam param;
  sceImeDialogParamInit(&param);
  param.type = SCE_IME_TYPE_DEFAULT;
  param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
  // The clear button doubles as "remove the label" - clearing the text and confirming renames
  // the backup back to its bare timestamp.
  param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
  param.title = title_utf16;
  param.maxTextLength = static_cast<SceUInt32>(
      std::min<std::size_t>(max_length, SCE_IME_DIALOG_MAX_TEXT_LENGTH));
  param.initialText = initial_utf16;
  param.inputTextBuffer = input_utf16;
  if (sceImeDialogInit(&param) < 0) {
    return TextInputResult::Failed;
  }

  bool accepted = false;
  while (true) {
    if (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_FINISHED) {
      SceImeDialogResult result{};
      sceImeDialogGetResult(&result);
      accepted = result.button == SCE_IME_DIALOG_BUTTON_ENTER;
      break;
    }
    // Same dimmed backdrop the busy modal uses; the dialog itself is composited by the common
    // dialog layer, which needs the update call between end_drawing and swap_buffers.
    vita2d_start_drawing();
    vita2d_clear_screen();
    draw_modal_backdrop();
    vita2d_end_drawing();
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
  }
  sceImeDialogTerm();

  // The press that closed the dialog is still down; returning now would hand the main loop a
  // fresh button edge (the confirm button doubles as Restore). Block until the pad is idle.
  SceCtrlData pad{};
  do {
    sceCtrlPeekBufferPositive(0, &pad, 1);
    sceKernelDelayThread(16 * 1000);
  } while (pad.buttons != 0);

  if (!accepted) {
    return TextInputResult::Canceled;
  }
  *out_text = utf16_to_utf8(input_utf16);
  return TextInputResult::Accepted;
}

} // namespace vsm::vita
