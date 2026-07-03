#include "vita/ui/Ui.hpp"

#include "core/BackupList.hpp"
#include "core/GoogleAuth.hpp"
#include "core/GridWindow.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
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
constexpr const char *kFallbackFontPath = "app0:sce_sys/resources/font.pgf";

constexpr unsigned int kTextSizeTiny = 15;
constexpr unsigned int kTextSizeSmall = 17;
constexpr unsigned int kTextSizeNormal = 18;
constexpr unsigned int kTextSizeTitle = 22;
constexpr unsigned int kTextSizeCode = 38;

enum class ButtonSymbol {
  Circle,
  Cross,
  Square,
  Triangle,
  Select,
  Start,
};

void draw_text(const FontSet &fonts, int x, int y, unsigned int color, unsigned int size,
               const char *text) {
  vita2d_font *font = size < FontSet::kMaxSize ? fonts.by_size[size] : nullptr;
  if (font) {
    vita2d_font_draw_text(font, x, y, color, size, text);
  } else if (fonts.fallback) {
    // The PGF fallback intentionally stays unscaled. Fractional PGF scaling is what looked bad on
    // hardware; it is better to keep fallback text consistent than to reintroduce that artifact.
    vita2d_pgf_draw_text(fonts.fallback, x, y, color, 1.0f, text);
  }
}

int text_width(const FontSet &fonts, unsigned int size, const char *text) {
  vita2d_font *font = size < FontSet::kMaxSize ? fonts.by_size[size] : nullptr;
  if (font) {
    return vita2d_font_text_width(font, size, text);
  }
  if (fonts.fallback) {
    return vita2d_pgf_text_width(fonts.fallback, 1.0f, text);
  }
  return 0;
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
  if (text.size() <= max_length) {
    return text;
  }
  if (max_length <= 3) {
    return text.substr(0, max_length);
  }
  return text.substr(0, max_length - 3) + "...";
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

int hint_width(const FontSet &fonts, ButtonSymbol symbol, const char *label) {
  const int label_w = text_width(fonts, kTextSizeSmall, label);
  if (symbol == ButtonSymbol::Select || symbol == ButtonSymbol::Start) {
    const char *pill_text = symbol == ButtonSymbol::Start ? "START" : "SEL";
    return text_width(fonts, kTextSizeTiny, pill_text) + 12 + 8 + label_w;
  }
  return 22 + label_w;
}

void draw_hint(const FontSet &fonts, int x, ButtonSymbol symbol, const char *label) {
  int label_x = x + 22;
  if (symbol == ButtonSymbol::Select || symbol == ButtonSymbol::Start) {
    const char *pill_text = symbol == ButtonSymbol::Start ? "START" : "SEL";
    // Size the pill from the measured text so the label never spills past its edge.
    const int pill_w = text_width(fonts, kTextSizeTiny, pill_text) + 12;
    vita2d_draw_rectangle(x, 516, pill_w, 20, RGBA8(255, 255, 255, 30));
    draw_text(fonts, x + 6, 531, kColorMuted, kTextSizeTiny, pill_text);
    label_x = x + pill_w + 8;
  } else {
    draw_button_shape(x, kFooterSymbolTop, symbol, kColorMuted);
  }
  draw_text(fonts, label_x, kFooterBaseline, kColorMuted, kTextSizeSmall, label);
}

// Lays hints out right-to-left so the primary action always sits at the right edge, the way the
// system UI arranges its button hints.
struct HintSpec {
  ButtonSymbol symbol;
  const char *label;
};

void draw_hints_right_aligned(const FontSet &fonts, const HintSpec *hints, int count) {
  int x = 936;
  for (int i = count - 1; i >= 0; --i) {
    x -= hint_width(fonts, hints[i].symbol, hints[i].label);
    draw_hint(fonts, x, hints[i].symbol, hints[i].label);
    x -= 36;
  }
}

std::string format_minutes_seconds(int total_seconds) {
  if (total_seconds < 0) {
    total_seconds = 0;
  }
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%d:%02d", total_seconds / 60, total_seconds % 60);
  return buffer;
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
  if (!any_font) {
    fonts_.fallback = vita2d_load_custom_pgf(kFallbackFontPath);
    if (!fonts_.fallback) {
      fonts_.fallback = vita2d_load_default_pgf();
    }
  }
  return any_font || fonts_.fallback != nullptr;
}

void Ui::shutdown() {
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
  vita2d_start_drawing();
  vita2d_clear_screen();

  draw_header(state);
  draw_title_grid(state);
  draw_backup_panel(state);
  draw_footer(state);

  vita2d_end_drawing();
  vita2d_swap_buffers();
}

void Ui::draw_header(const UiState &state) {
  vita2d_draw_rectangle(0, 0, 960, 52, kColorHeader);
  vita2d_draw_line(0, 52, 960, 52, RGBA8(255, 255, 255, 20));

  draw_text(fonts_, 18, 34, kColorText, kTextSizeTitle, "Save Keeper");

  const char *drive_status = state.google_auth_pending ? "Connecting to Google..."
                             : !state.google_connected ? "Google Drive not connected"
                             : state.drive_synced      ? "Google Drive synced"
                                                       : "Google Drive connected";
  const unsigned int dot_color = state.google_connected      ? kColorSuccess
                                 : state.google_auth_pending ? kColorPendingDot
                                                             : kColorIdleDot;
  const int status_x = 960 - 18 - measure_text(kTextSizeSmall, drive_status);
  vita2d_draw_fill_circle(status_x - 14, 26, 5, dot_color);
  draw_text(fonts_, status_x, 32, kColorMuted, kTextSizeSmall, drive_status);
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

  // Selected-save details live here, in one place, above its backup menu.
  const std::string title = truncate_label(save->display_name, 30);
  const std::string details = truncate_label(
      title_id_label(*save) + "  |  " + platform_label(save->platform) + " save", 44);
  // Baseline 84 matches the tab row in the left pane so the top lines read as one row.
  draw_text(fonts_, 528, 84, kColorText, kTextSizeNormal, title.c_str());
  draw_text(fonts_, 528, 110, kColorMuted, kTextSizeSmall, details.c_str());

  constexpr std::size_t kVisibleEntries = 7;
  const std::vector<BackupEntry> all_entries =
      build_backup_menu(state.remote_backups, *state.local_backups);
  // Menu indices match the App: 0 is the always-selectable "New Backup" entry. The window only
  // scrolls when the selection reaches its edge, like the save grid.
  const std::size_t selected_entry = state.selected_backup;
  backup_top_row_ = grid_window_top_row(backup_top_row_, selected_entry, all_entries.size(), 1,
                                        kVisibleEntries);
  const std::size_t backup_window = backup_top_row_;

  std::vector<BackupEntry> entries;
  for (std::size_t i = backup_window;
       i < all_entries.size() && entries.size() < kVisibleEntries; ++i) {
    entries.push_back(all_entries[i]);
  }

  int y = 136;
  const std::size_t visible_count = entries.size();
  for (std::size_t i = 0; i < visible_count; ++i) {
    const bool selected = (i + backup_window) == selected_entry;
    const unsigned int bg = selected ? kColorAccentSoft : RGBA8(255, 255, 255, 18);
    vita2d_draw_rectangle(528, y, 408, 36, bg);
    if (selected) {
      vita2d_draw_rectangle(528, y, 4, 36, kColorAccent);
    }

    const std::string label = truncate_label(entries[i].display_name(), 42);
    draw_text(fonts_, 542, y + 24, selected ? kColorText : kColorMuted,
              kTextSizeSmall, label.c_str());
    y += 42;
  }

  if (backup_window > 0) {
    // Chevrons say "more above/below" without spending a text row on them.
    vita2d_draw_line(714, 132, 720, 126, kColorMuted);
    vita2d_draw_line(720, 126, 726, 132, kColorMuted);
  }
  if (all_entries.size() > backup_window + entries.size()) {
    vita2d_draw_line(714, 436, 720, 442, kColorMuted);
    vita2d_draw_line(720, 442, 726, 436, kColorMuted);
  }

  if (state.remote_backups.size() + state.local_backups->size() > 0) {
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
  draw_text(fonts_, 528, 246, kColorMuted, kTextSizeTiny,
            truncate_label(url, 40).c_str());
  draw_text(fonts_, 528, 300, kColorAccent, kTextSizeCode,
            state.google_user_code.c_str());

  // The QR code carries the verification URL with the user code pre-filled, so scanning skips the
  // typing step entirely.
  const std::string qr_url = build_device_verification_qr_url(state.google_verification_url,
                                                              state.google_user_code);
  draw_qr_code(qr_url, 804, 104, 4);

  // A lightweight pulse so the wait feels alive: one dot per half second, cycling.
  const int dot_count = 1 + static_cast<int>((frame_counter_ / 30U) % 3U);
  std::string waiting = "Waiting for approval";
  for (int i = 0; i < dot_count; ++i) {
    waiting.push_back('.');
  }
  draw_text(fonts_, 528, 360, kColorMuted, kTextSizeNormal, waiting.c_str());

  const std::string validity =
      "Code valid for " + format_minutes_seconds(state.auth_seconds_left);
  draw_text(fonts_, 528, 388, kColorMuted, kTextSizeTiny, validity.c_str());
}

void Ui::draw_status_line(const UiState &state) {
  if (state.status_message.empty()) {
    return;
  }
  const std::string status = truncate_label(state.status_message, 48);
  unsigned int color = kColorMuted;
  if (state.restore_confirmation_pending || state.delete_confirmation_pending) {
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
}

void Ui::draw_footer(const UiState &state) {
  vita2d_draw_rectangle(0, 508, 960, 36, RGBA8(3, 7, 18, 230));

  const ButtonSymbol confirm = state.enter_is_cross ? ButtonSymbol::Cross : ButtonSymbol::Circle;
  const ButtonSymbol cancel = state.enter_is_cross ? ButtonSymbol::Circle : ButtonSymbol::Cross;

  // The footer follows the current mode so a cancel hint only appears when there is actually
  // something to cancel. Hints are right-aligned with the primary action at the right edge.
  if (state.google_auth_pending) {
    const HintSpec hints[] = {{cancel, "Cancel sign-in"}, {ButtonSymbol::Triangle, "Check now"}};
    draw_hints_right_aligned(fonts_, hints, 2);
    return;
  }
  if (state.restore_confirmation_pending) {
    const HintSpec hints[] = {{cancel, "Cancel"}, {confirm, "Confirm restore"}};
    draw_hints_right_aligned(fonts_, hints, 2);
    return;
  }
  if (state.delete_confirmation_pending) {
    const HintSpec hints[] = {{cancel, "Cancel"}, {ButtonSymbol::Start, "Confirm delete"}};
    draw_hints_right_aligned(fonts_, hints, 2);
    return;
  }

  // One context action: create a snapshot on the "New Backup" entry, restore on a backup entry.
  const HintSpec hints[] = {
      {ButtonSymbol::Triangle, state.google_connected ? "Refresh" : "Google"},
      {ButtonSymbol::Start, "Delete"},
      {ButtonSymbol::Select, "Upload"},
      {confirm, state.selected_backup == 0 ? "Backup" : "Restore"},
  };
  draw_hints_right_aligned(fonts_, hints, 4);
}

void Ui::draw_busy(const std::string &label, long long done, long long total) {
  ++frame_counter_;
  vita2d_start_drawing();
  vita2d_clear_screen();

  constexpr int kBoxX = 280;
  constexpr int kBoxY = 216;
  constexpr int kBoxW = 400;
  constexpr int kBoxH = 112;
  vita2d_draw_rectangle(kBoxX, kBoxY, kBoxW, kBoxH, kColorPanelAlt);
  vita2d_draw_rectangle(kBoxX, kBoxY, kBoxW, 4, kColorAccent);

  draw_text(fonts_, kBoxX + 24, kBoxY + 42, kColorText, kTextSizeNormal,
            label.c_str());

  const int bar_x = kBoxX + 24;
  const int bar_y = kBoxY + 62;
  const int bar_w = kBoxW - 48;
  const int bar_h = 12;
  vita2d_draw_rectangle(bar_x, bar_y, bar_w, bar_h, RGBA8(255, 255, 255, 24));

  if (total > 0) {
    float fraction = static_cast<float>(done) / static_cast<float>(total);
    fraction = std::max(0.0f, std::min(1.0f, fraction));
    vita2d_draw_rectangle(bar_x, bar_y, static_cast<int>(bar_w * fraction), bar_h, kColorAccent);
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(fraction * 100.0f));
    draw_text(fonts_, bar_x, kBoxY + 98, kColorMuted, kTextSizeSmall, percent);
  } else {
    // Unknown size: sweep a segment across the bar so the app visibly keeps working.
    const int segment = bar_w / 4;
    const int travel = bar_w - segment;
    const int offset = static_cast<int>((frame_counter_ * 8U) % static_cast<unsigned int>(travel * 2));
    const int start = offset <= travel ? offset : travel * 2 - offset;
    vita2d_draw_rectangle(bar_x + start, bar_y, segment, bar_h, kColorAccent);
  }

  vita2d_end_drawing();
  vita2d_swap_buffers();
}

} // namespace vsm::vita
