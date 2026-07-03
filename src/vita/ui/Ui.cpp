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

constexpr unsigned int kTextSizeTiny = 14;
constexpr unsigned int kTextSizeSmall = 16;
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

void draw_text(vita2d_font *font, vita2d_pgf *fallback_font, int x, int y,
               unsigned int color, unsigned int size, const char *text) {
  if (font) {
    vita2d_font_draw_text(font, x, y, color, size, text);
  } else if (fallback_font) {
    // The PGF fallback intentionally stays unscaled. Fractional PGF scaling is what looked bad on
    // hardware; it is better to keep fallback text consistent than to reintroduce that artifact.
    vita2d_pgf_draw_text(fallback_font, x, y, color, 1.0f, text);
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

void draw_placeholder_icon(vita2d_font *font, vita2d_pgf *fallback_font,
                           const SaveRecord &save, int x, int y, int size, bool selected) {
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
  draw_text(font, fallback_font, x + 18, y + size - 14,
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

void draw_hint(vita2d_font *font, vita2d_pgf *fallback_font, int x, ButtonSymbol symbol,
               const char *label) {
  const int y = 518;
  int label_x = x + 22;
  if (symbol == ButtonSymbol::Select || symbol == ButtonSymbol::Start) {
    const bool is_start = symbol == ButtonSymbol::Start;
    const int pill_w = is_start ? 52 : 38;
    vita2d_draw_rectangle(x, y - 8, pill_w, 18, RGBA8(255, 255, 255, 30));
    draw_text(font, fallback_font, x + 6, y + 5, kColorMuted, kTextSizeTiny,
              is_start ? "START" : "SEL");
    label_x = x + pill_w + 8;
  } else {
    draw_button_shape(x, y - 10, symbol, kColorMuted);
  }
  draw_text(font, fallback_font, label_x, y + 4, kColorMuted, kTextSizeSmall, label);
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
  font_ = vita2d_load_font_file(kBundledFontPath);
  if (!font_) {
    fallback_font_ = vita2d_load_custom_pgf(kFallbackFontPath);
    if (!fallback_font_) {
      fallback_font_ = vita2d_load_default_pgf();
    }
  }
  return font_ != nullptr || fallback_font_ != nullptr;
}

void Ui::shutdown() {
  for (auto &entry : icon_cache_) {
    if (entry.second) {
      vita2d_free_texture(entry.second);
    }
  }
  icon_cache_.clear();

  if (font_) {
    vita2d_free_font(font_);
    font_ = nullptr;
  }
  if (fallback_font_) {
    vita2d_free_pgf(fallback_font_);
    fallback_font_ = nullptr;
  }
  vita2d_fini();
}

int Ui::measure_text(unsigned int size, const char *text) const {
  if (font_) {
    return vita2d_font_text_width(font_, size, text);
  }
  if (fallback_font_) {
    return vita2d_pgf_text_width(fallback_font_, 1.0f, text);
  }
  return 0;
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

  draw_text(font_, fallback_font_, 18, 34, kColorText, kTextSizeTitle, "Save Keeper");

  const char *drive_status = state.google_connected      ? "Google Drive connected"
                             : state.google_auth_pending ? "Connecting to Google..."
                                                         : "Google Drive not connected";
  const unsigned int dot_color = state.google_connected      ? kColorSuccess
                                 : state.google_auth_pending ? kColorPendingDot
                                                             : kColorIdleDot;
  vita2d_draw_fill_circle(638, 26, 5, dot_color);
  draw_text(font_, fallback_font_, 654, 32, kColorMuted, kTextSizeSmall, drive_status);
}

void Ui::draw_title_grid(const UiState &state) {
  const std::vector<SaveRecord> &saves = *state.saves;
  const std::vector<std::size_t> &visible = *state.visible_saves;
  vita2d_draw_rectangle(0, 52, 432, 456, kColorPanel);
  vita2d_draw_line(432, 52, 432, 508, RGBA8(255, 255, 255, 20));

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
    draw_text(font_, fallback_font_, tab_x, 84, color, kTextSizeSmall, label.c_str());
    const int width = measure_text(kTextSizeSmall, label.c_str());
    if (active) {
      vita2d_draw_rectangle(tab_x, 90, width, 3, kColorAccent);
    }
    tab_x += width + 20;
  }
  // Small L/R chips around the tab row show which buttons switch groups.
  draw_text(font_, fallback_font_, 16, 84, kColorIdleDot, kTextSizeTiny, "L");
  draw_text(font_, fallback_font_, tab_x, 84, kColorIdleDot, kTextSizeTiny, "R");

  constexpr int kColumns = 4;
  constexpr int kRows = 3;
  constexpr int kTileSize = 96;
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
    draw_text(font_, fallback_font_, 432 - 16 - measure_text(kTextSizeSmall, count.c_str()), 84,
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
        draw_placeholder_icon(font_, fallback_font_, save, x + pad, y + pad, icon_size,
                              is_selected);
      }
    }
  }

  if (!visible.empty()) {
    const SaveRecord &save = saves[visible[selected]];
    const std::string title = truncate_label(save.display_name, 34);
    const std::string id = truncate_label(title_id_label(save), 42);
    const std::string platform = std::string(platform_label(save.platform)) + " save";
    draw_text(font_, fallback_font_, 18, 444, kColorText, kTextSizeNormal, title.c_str());
    draw_text(font_, fallback_font_, 18, 470, kColorMuted, kTextSizeSmall, id.c_str());
    draw_text(font_, fallback_font_, 18, 492, kColorMuted, kTextSizeTiny, platform.c_str());
  } else if (saves.empty()) {
    draw_text(font_, fallback_font_, 18, 430, kColorText, kTextSizeNormal, "No saves found");
    draw_text(font_, fallback_font_, 18, 456, kColorMuted, kTextSizeSmall,
              "Checked Vita, game card, and PSP roots");
  } else {
    draw_text(font_, fallback_font_, 18, 430, kColorText, kTextSizeNormal,
              "No saves in this group");
    draw_text(font_, fallback_font_, 18, 456, kColorMuted, kTextSizeSmall,
              "L / R switches groups");
  }
}

void Ui::draw_backup_panel(const UiState &state) {
  vita2d_draw_rectangle(432, 52, 528, 456, RGBA8(15, 23, 42, 255));

  if (state.google_auth_pending && !state.google_user_code.empty()) {
    draw_google_auth_panel(state);
    draw_status_line(state);
    return;
  }

  draw_text(font_, fallback_font_, 456, 84, kColorText, kTextSizeNormal, "Backups");
  const SaveRecord *save = selected_visible_record(state);

  const char *google_action = state.google_connected
                                  ? "Triangle refreshes [GD] remote backups"
                                  : "Triangle connects Google Drive";
  draw_text(font_, fallback_font_, 456, 112, kColorMuted, kTextSizeSmall, google_action);

  if (state.remote_backups.size() + state.local_backups->size() > 0) {
    draw_text(font_, fallback_font_, 796, 84, kColorMuted, kTextSizeTiny, "R Stick selects");
  }

  if (!save) {
    draw_text(font_, fallback_font_, 456, 150, kColorMuted, kTextSizeSmall,
              "Install or create saves, then reopen Save Keeper.");
    draw_status_line(state);
    return;
  }

  const std::string selected_label = truncate_label(save->display_name, 36);
  draw_text(font_, fallback_font_, 456, 138, kColorMuted, kTextSizeSmall,
            selected_label.c_str());

  constexpr std::size_t kVisibleBackups = 5;
  const std::vector<BackupEntry> all_entries =
      build_backup_menu(state.remote_backups, *state.local_backups);
  const std::size_t selectable_count =
      state.remote_backups.size() + state.local_backups->size();
  const std::size_t selected_entry = selectable_count == 0 ? 0 : 1 + state.selected_backup;
  const std::size_t backup_window =
      all_entries.size() <= kVisibleBackups + 1
          ? 0
          : std::min(selected_entry, all_entries.size() - (kVisibleBackups + 1));

  std::vector<BackupEntry> entries;
  for (std::size_t i = backup_window;
       i < all_entries.size() && entries.size() < kVisibleBackups + 1; ++i) {
    entries.push_back(all_entries[i]);
  }

  int y = 164;
  const std::size_t visible_count = entries.size();
  for (std::size_t i = 0; i < visible_count; ++i) {
    const bool selected = (i + backup_window) == selected_entry;
    const unsigned int bg = selected ? kColorAccentSoft : RGBA8(255, 255, 255, 18);
    vita2d_draw_rectangle(456, y, 460, 36, bg);
    if (selected) {
      vita2d_draw_rectangle(456, y, 4, 36, kColorAccent);
    }

    const std::string label = truncate_label(entries[i].display_name(), 46);
    draw_text(font_, fallback_font_, 470, y + 24, selected ? kColorText : kColorMuted,
              kTextSizeSmall, label.c_str());
    y += 42;
  }

  if (selectable_count == 0) {
    draw_text(font_, fallback_font_, 470, 230, kColorMuted, kTextSizeSmall, "No backups yet");
  } else if (all_entries.size() > entries.size()) {
    draw_text(font_, fallback_font_, 470, 414, kColorMuted, kTextSizeSmall,
              "More backups below");
  }

  draw_status_line(state);
}

void Ui::draw_google_auth_panel(const UiState &state) {
  draw_text(font_, fallback_font_, 456, 90, kColorText, kTextSizeTitle, "Connect Google Drive");

  draw_text(font_, fallback_font_, 456, 138, kColorText, kTextSizeNormal,
            "1  Scan the QR code with your phone");
  draw_text(font_, fallback_font_, 456, 168, kColorText, kTextSizeNormal,
            "2  Sign in and approve Save Keeper");
  draw_text(font_, fallback_font_, 456, 198, kColorText, kTextSizeNormal,
            "3  This screen finishes by itself");

  // Keep this line short: the QR block starts at x=776, and a longer sentence would run into it.
  const std::string url = "Or visit " + display_url(state.google_verification_url) + " and enter:";
  draw_text(font_, fallback_font_, 456, 246, kColorMuted, kTextSizeSmall,
            truncate_label(url, 40).c_str());
  draw_text(font_, fallback_font_, 456, 300, kColorAccent, kTextSizeCode,
            state.google_user_code.c_str());

  // The QR code carries the verification URL with the user code pre-filled, so scanning skips the
  // typing step entirely.
  const std::string qr_url = build_device_verification_qr_url(state.google_verification_url,
                                                              state.google_user_code);
  draw_qr_code(qr_url, 776, 108, 4);

  // A lightweight pulse so the wait feels alive: one dot per half second, cycling.
  const int dot_count = 1 + static_cast<int>((frame_counter_ / 30U) % 3U);
  std::string waiting = "Waiting for approval";
  for (int i = 0; i < dot_count; ++i) {
    waiting.push_back('.');
  }
  draw_text(font_, fallback_font_, 456, 360, kColorMuted, kTextSizeNormal, waiting.c_str());

  const std::string validity =
      "Code valid for " + format_minutes_seconds(state.auth_seconds_left);
  draw_text(font_, fallback_font_, 456, 388, kColorMuted, kTextSizeTiny, validity.c_str());
}

void Ui::draw_status_line(const UiState &state) {
  const std::string status =
      state.status_message.empty() ? "Circle creates a timestamped ZIP snapshot."
                                   : truncate_label(state.status_message, 58);
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
  draw_text(font_, fallback_font_, 456, 462, color, kTextSizeSmall, status.c_str());
}

void Ui::draw_footer(const UiState &state) {
  vita2d_draw_rectangle(0, 508, 960, 36, RGBA8(3, 7, 18, 230));

  const ButtonSymbol confirm = state.enter_is_cross ? ButtonSymbol::Cross : ButtonSymbol::Circle;
  const ButtonSymbol cancel = state.enter_is_cross ? ButtonSymbol::Circle : ButtonSymbol::Cross;

  // The footer follows the current mode so a cancel hint only appears when there is actually
  // something to cancel.
  if (state.google_auth_pending) {
    draw_hint(font_, fallback_font_, 28, ButtonSymbol::Triangle, "Check now");
    draw_hint(font_, fallback_font_, 210, cancel, "Cancel sign-in");
    return;
  }
  if (state.restore_confirmation_pending) {
    draw_hint(font_, fallback_font_, 28, ButtonSymbol::Square, "Confirm restore");
    draw_hint(font_, fallback_font_, 240, cancel, "Cancel");
    return;
  }
  if (state.delete_confirmation_pending) {
    draw_hint(font_, fallback_font_, 28, ButtonSymbol::Start, "Confirm delete");
    draw_hint(font_, fallback_font_, 240, cancel, "Cancel");
    return;
  }

  draw_hint(font_, fallback_font_, 28, confirm, "Backup");
  draw_hint(font_, fallback_font_, 156, ButtonSymbol::Square, "Restore");
  draw_hint(font_, fallback_font_, 292, ButtonSymbol::Select, "Upload");
  draw_hint(font_, fallback_font_, 436, ButtonSymbol::Start, "Delete");
  draw_hint(font_, fallback_font_, 592, ButtonSymbol::Triangle,
            state.google_connected ? "Refresh" : "Google");
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

  draw_text(font_, fallback_font_, kBoxX + 24, kBoxY + 42, kColorText, kTextSizeNormal,
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
    draw_text(font_, fallback_font_, bar_x, kBoxY + 98, kColorMuted, kTextSizeSmall, percent);
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
