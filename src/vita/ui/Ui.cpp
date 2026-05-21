#include "vita/ui/Ui.hpp"

#include "core/BackupList.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
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

constexpr float kTextScaleSmall = 0.82f;
constexpr float kTextScaleNormal = 0.98f;
constexpr float kTextScaleTitle = 1.16f;

enum class ButtonSymbol {
  Circle,
  Cross,
  Square,
  Triangle,
  Select,
};

void draw_text(vita2d_pvf *font, int x, int y, unsigned int color, float scale,
               const char *text) {
  if (font) {
    vita2d_pvf_draw_text(font, x, y, color, scale, text);
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

const SaveRecord *selected_record(const std::vector<SaveRecord> &saves,
                                  std::size_t selected_save) {
  if (saves.empty()) {
    return nullptr;
  }
  return &saves[selected_save % saves.size()];
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

void draw_qr_code(const std::string &value, int x, int y, int module_size) {
  if (value.empty()) {
    return;
  }

  const qrcodegen::QrCode qr =
      qrcodegen::QrCode::encodeText(value.c_str(), qrcodegen::QrCode::Ecc::LOW);
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

void draw_placeholder_icon(vita2d_pvf *font, const SaveRecord &save, int x, int y, int size,
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
  draw_text(font, x + 22, y + size - 12, selected ? kColorText : RGBA8(198, 210, 226, 255), 0.82f,
            text.c_str());
}

void draw_button_shape(int x, int y, ButtonSymbol symbol, unsigned int color,
                       vita2d_pvf *font) {
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
    vita2d_draw_rectangle(x, y + 2, 36, 14, RGBA8(255, 255, 255, 18));
    draw_text(font, x + 5, y + 14, color, 0.52f, "SEL");
    break;
  }
}

void draw_hint(vita2d_pvf *font, int x, ButtonSymbol symbol, const char *label) {
  const int y = 518;
  draw_button_shape(x, y - 10, symbol, kColorMuted, font);
  const int label_x = symbol == ButtonSymbol::Select ? x + 42 : x + 22;
  draw_text(font, label_x, y + 4, kColorMuted, 0.68f, label);
}

} // namespace

bool Ui::initialize() {
  if (vita2d_init() < 0) {
    return false;
  }

  vita2d_set_clear_color(kColorBackground);
  font_ = vita2d_load_default_pvf();
  return font_ != nullptr;
}

void Ui::shutdown() {
  for (auto &entry : icon_cache_) {
    if (entry.second) {
      vita2d_free_texture(entry.second);
    }
  }
  icon_cache_.clear();

  if (font_) {
    vita2d_free_pvf(font_);
    font_ = nullptr;
  }
  vita2d_fini();
}

vita2d_texture *Ui::load_icon_texture(const std::string &path) const {
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

void Ui::draw(const std::vector<SaveRecord> &saves, std::size_t selected_save,
              const std::vector<std::string> &remote_backups,
              const std::vector<std::string> &local_backups, std::size_t selected_backup,
              bool restore_confirmation_pending, bool google_connected, bool google_auth_pending,
              const std::string &google_verification_url, const std::string &google_user_code,
              const std::string &status_message) {
  vita2d_start_drawing();
  vita2d_clear_screen();

  draw_header(google_connected, google_auth_pending);
  draw_title_grid(saves, selected_save);
  draw_backup_panel(saves, selected_save, remote_backups, local_backups, selected_backup,
                    restore_confirmation_pending, google_connected, google_auth_pending,
                    google_verification_url, google_user_code, status_message);
  draw_footer();

  vita2d_end_drawing();
  vita2d_swap_buffers();
}

void Ui::draw_header(bool google_connected, bool google_auth_pending) const {
  vita2d_draw_rectangle(0, 0, 960, 52, kColorHeader);
  vita2d_draw_line(0, 52, 960, 52, RGBA8(255, 255, 255, 20));

  draw_text(font_, 18, 34, kColorText, kTextScaleTitle, "Save Keeper");
  const char *drive_status = google_connected       ? "Google Drive connected"
                             : google_auth_pending ? "Google auth pending"
                                                   : "Google Drive not connected";
  draw_text(font_, 676, 32, kColorMuted, kTextScaleSmall, drive_status);
}

void Ui::draw_title_grid(const std::vector<SaveRecord> &saves, std::size_t selected_save) const {
  vita2d_draw_rectangle(0, 52, 432, 456, kColorPanel);
  vita2d_draw_line(432, 52, 432, 508, RGBA8(255, 255, 255, 20));

  draw_text(font_, 18, 84, kColorText, kTextScaleNormal, "Saves");

  constexpr int kColumns = 4;
  constexpr int kRows = 3;
  constexpr int kTileSize = 96;
  constexpr int kGapX = 8;
  constexpr int kGapY = 12;
  constexpr int kStartX = 16;
  constexpr int kStartY = 104;
  const std::size_t selected = saves.empty() ? 0 : selected_save % saves.size();
  const std::size_t selected_row = selected / kColumns;
  const std::size_t total_rows = (saves.size() + kColumns - 1) / kColumns;
  const std::size_t max_top_row =
      total_rows <= kRows ? 0 : static_cast<std::size_t>(total_rows - kRows);
  const std::size_t top_row =
      selected_row < kRows ? 0 : std::min(selected_row - kRows + 1, max_top_row);
  const std::size_t first_index = top_row * kColumns;

  if (!saves.empty()) {
    const std::string count =
        std::to_string(selected + 1) + "/" + std::to_string(saves.size());
    draw_text(font_, 350, 84, kColorMuted, kTextScaleSmall, count.c_str());
  }

  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kColumns; ++col) {
      const std::size_t index = first_index + static_cast<std::size_t>(row * kColumns + col);
      const int x = kStartX + col * (kTileSize + kGapX);
      const int y = kStartY + row * (kTileSize + kGapY);
      if (index >= saves.size()) {
        vita2d_draw_rectangle(x, y, kTileSize, kTileSize, RGBA8(255, 255, 255, 8));
        continue;
      }

      const bool is_selected = index == selected;
      const SaveRecord &save = saves[index];
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
        draw_placeholder_icon(font_, save, x + pad, y + pad, icon_size, is_selected);
      }
    }
  }

  const SaveRecord *save = selected_record(saves, selected_save);
  if (save) {
    const std::string title = truncate_label(save->display_name, 34);
    const std::string id = truncate_label(title_id_label(*save), 42);
    const std::string platform = std::string(platform_label(save->platform)) + " save";
    draw_text(font_, 18, 444, kColorText, kTextScaleNormal, title.c_str());
    draw_text(font_, 18, 470, kColorMuted, kTextScaleSmall, id.c_str());
    draw_text(font_, 18, 492, kColorMuted, 0.74f, platform.c_str());
  } else {
    draw_text(font_, 18, 430, kColorText, kTextScaleNormal, "No saves found");
    draw_text(font_, 18, 456, kColorMuted, kTextScaleSmall, "Checked Vita, game card, and PSP roots");
  }
}

void Ui::draw_backup_panel(const std::vector<SaveRecord> &saves, std::size_t selected_save,
                           const std::vector<std::string> &remote_backups,
                           const std::vector<std::string> &local_backups,
                           std::size_t selected_backup, bool restore_confirmation_pending,
                           bool google_connected, bool google_auth_pending,
                           const std::string &google_verification_url,
                           const std::string &google_user_code,
                           const std::string &status_message) const {
  vita2d_draw_rectangle(432, 52, 528, 456, RGBA8(15, 23, 42, 255));
  draw_text(font_, 456, 84, kColorText, kTextScaleNormal, "Backups");
  const SaveRecord *save = selected_record(saves, selected_save);

  const char *google_action = "Triangle connects or refreshes Google Drive";
  if (!google_user_code.empty()) {
    google_action = "Scan QR, approve Google, then press Triangle";
  } else if (google_auth_pending) {
    google_action = "Waiting for Google approval";
  } else if (google_connected) {
    google_action = "Triangle refreshes [GD] remote backups";
  }
  draw_text(font_, 456, 112, kColorMuted, 0.78f, google_action);

  if (!save) {
    draw_text(font_, 456, 150, kColorMuted, kTextScaleSmall,
              "Install or create saves, then reopen Save Keeper.");
    if (!status_message.empty()) {
      const std::string status = truncate_label(status_message, 58);
      draw_text(font_, 456, 462, kColorMuted, kTextScaleSmall, status.c_str());
    }
    return;
  }

  const std::string selected_label = truncate_label(save->display_name, 36);
  draw_text(font_, 456, 138, kColorMuted, kTextScaleSmall, selected_label.c_str());

  if (!google_user_code.empty()) {
    const std::string url = truncate_label(google_verification_url, 38);
    const std::string code = "Code: " + google_user_code;
    draw_text(font_, 470, 184, kColorText, 0.84f, "1 Scan the QR code");
    draw_text(font_, 470, 214, kColorText, 0.84f, "2 Approve Google Drive access");
    draw_text(font_, 470, 244, kColorText, 0.84f, "3 Press Triangle again here");
    draw_text(font_, 470, 306, kColorMuted, 0.76f, url.c_str());
    draw_text(font_, 470, 338, kColorAccent, 0.88f, code.c_str());
    draw_qr_code(google_verification_url, 780, 166, 3);
    return;
  }

  constexpr std::size_t kVisibleBackups = 5;
  const std::vector<BackupEntry> all_entries = build_backup_menu(remote_backups, local_backups);
  const std::size_t selectable_count = remote_backups.size() + local_backups.size();
  const std::size_t selected_entry = selectable_count == 0 ? 0 : 1 + selected_backup;
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
    draw_text(font_, 470, y + 24, selected ? kColorText : kColorMuted, 0.78f, label.c_str());
    y += 42;
  }

  if (selectable_count == 0) {
    draw_text(font_, 470, 230, kColorMuted, kTextScaleSmall, "No backups yet");
  } else if (all_entries.size() > entries.size() && google_user_code.empty()) {
    draw_text(font_, 470, 414, kColorMuted, kTextScaleSmall, "More backups below");
  }

  const std::string status =
      status_message.empty() ? "Circle creates a timestamped ZIP snapshot."
                             : truncate_label(status_message, 58);
  draw_text(font_, 456, 462,
            restore_confirmation_pending ? kColorAccent : kColorMuted, kTextScaleSmall,
            status.c_str());
}

void Ui::draw_footer() const {
  vita2d_draw_rectangle(0, 508, 960, 36, RGBA8(3, 7, 18, 230));
  draw_text(font_, 18, 532, kColorMuted, 0.68f, "L/R Backups");
  draw_hint(font_, 146, ButtonSymbol::Circle, "Backup");
  draw_hint(font_, 268, ButtonSymbol::Square, "Restore");
  draw_hint(font_, 402, ButtonSymbol::Select, "Upload");
  draw_hint(font_, 552, ButtonSymbol::Triangle, "Google");
  draw_hint(font_, 688, ButtonSymbol::Cross, "Cancel");
}

} // namespace vsm::vita
