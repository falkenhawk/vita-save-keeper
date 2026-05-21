#include "vita/ui/Ui.hpp"

#include "core/BackupList.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>
#include <vita2d.h>

namespace vsm::vita {
namespace {

constexpr unsigned int kColorBackground = RGBA8(11, 15, 21, 255);
constexpr unsigned int kColorHeader = RGBA8(26, 35, 50, 255);
constexpr unsigned int kColorPanel = RGBA8(21, 29, 42, 255);
constexpr unsigned int kColorPanelAlt = RGBA8(31, 42, 59, 255);
constexpr unsigned int kColorAccent = RGBA8(56, 189, 248, 255);
constexpr unsigned int kColorAccentSoft = RGBA8(56, 189, 248, 48);
constexpr unsigned int kColorText = RGBA8(232, 238, 246, 255);
constexpr unsigned int kColorMuted = RGBA8(148, 163, 184, 255);

constexpr float kTextScaleSmall = 0.78f;
constexpr float kTextScaleNormal = 0.92f;
constexpr float kTextScaleTitle = 1.15f;

void draw_text(vita2d_pgf *font, int x, int y, unsigned int color, float scale,
               const char *text) {
  if (font) {
    vita2d_pgf_draw_text(font, x, y, color, scale, text);
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

} // namespace

bool Ui::initialize() {
  if (vita2d_init() < 0) {
    return false;
  }

  vita2d_set_clear_color(kColorBackground);
  font_ = vita2d_load_default_pgf();
  return font_ != nullptr;
}

void Ui::shutdown() {
  if (font_) {
    vita2d_free_pgf(font_);
    font_ = nullptr;
  }
  vita2d_fini();
}

void Ui::draw(const std::vector<SaveRecord> &saves, std::size_t selected_save,
              const std::string &status_message) {
  vita2d_start_drawing();
  vita2d_clear_screen();

  draw_header();
  draw_title_grid(saves, selected_save);
  draw_backup_panel(saves, selected_save, status_message);
  draw_footer();

  vita2d_end_drawing();
  vita2d_swap_buffers();
}

void Ui::draw_header() const {
  vita2d_draw_rectangle(0, 0, 960, 52, kColorHeader);
  vita2d_draw_line(0, 52, 960, 52, RGBA8(255, 255, 255, 28));

  draw_text(font_, 18, 34, kColorText, kTextScaleTitle, "Save Keeper");
  draw_text(font_, 700, 32, kColorMuted, kTextScaleSmall, "Google Drive not connected");
}

void Ui::draw_title_grid(const std::vector<SaveRecord> &saves, std::size_t selected_save) const {
  vita2d_draw_rectangle(0, 52, 432, 456, kColorPanel);
  vita2d_draw_line(432, 52, 432, 508, RGBA8(255, 255, 255, 28));

  draw_text(font_, 18, 84, kColorText, kTextScaleNormal, "Saves");

  constexpr int kColumns = 4;
  constexpr int kRows = 3;
  constexpr int kTileSize = 82;
  constexpr int kGap = 14;
  constexpr int kStartX = 22;
  constexpr int kStartY = 108;
  constexpr int kVisibleTiles = kColumns * kRows;
  const std::size_t selected = saves.empty() ? 0 : selected_save % saves.size();

  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kColumns; ++col) {
      const int index = row * kColumns + col;
      const int x = kStartX + col * (kTileSize + kGap);
      const int y = kStartY + row * (kTileSize + kGap);
      const bool has_save = static_cast<std::size_t>(index) < saves.size();
      const bool is_selected = has_save && static_cast<std::size_t>(index) == selected;

      vita2d_draw_rectangle(x - 3, y - 3, kTileSize + 6, kTileSize + 6,
                            is_selected ? kColorAccent : RGBA8(255, 255, 255, 20));
      vita2d_draw_rectangle(x, y, kTileSize, kTileSize,
                            is_selected ? RGBA8(219, 234, 254, 255) : kColorPanelAlt);

      if (has_save) {
        const SaveRecord &save = saves[static_cast<std::size_t>(index)];
        const std::string id = truncate_label(save.id, 10);
        const unsigned int text_color = is_selected ? RGBA8(15, 23, 42, 255) : kColorMuted;
        draw_text(font_, x + 8, y + 34, text_color, 0.62f, platform_label(save.platform));
        draw_text(font_, x + 8, y + 58, text_color, 0.56f, id.c_str());
      }
    }
  }

  const SaveRecord *save = selected_record(saves, selected_save);
  if (save) {
    const std::string title = truncate_label(save->display_name, 34);
    const std::string path = truncate_label(save->path, 46);
    draw_text(font_, 18, 430, kColorText, kTextScaleNormal, title.c_str());
    draw_text(font_, 18, 456, kColorMuted, kTextScaleSmall, path.c_str());
  } else {
    draw_text(font_, 18, 430, kColorText, kTextScaleNormal, "No saves found");
    draw_text(font_, 18, 456, kColorMuted, kTextScaleSmall, "Checked ux0 Vita and Adrenaline roots");
  }

  if (saves.size() > static_cast<std::size_t>(kVisibleTiles)) {
    draw_text(font_, 300, 84, kColorMuted, kTextScaleSmall, "More saves hidden");
  }
}

void Ui::draw_backup_panel(const std::vector<SaveRecord> &saves, std::size_t selected_save,
                           const std::string &status_message) const {
  vita2d_draw_rectangle(432, 52, 528, 456, RGBA8(15, 23, 42, 255));
  draw_text(font_, 456, 84, kColorText, kTextScaleNormal, "Backups");
  const SaveRecord *save = selected_record(saves, selected_save);

  if (!save) {
    draw_text(font_, 456, 128, kColorMuted, kTextScaleSmall,
              "Install or create saves, then reopen Save Keeper.");
    if (!status_message.empty()) {
      const std::string status = truncate_label(status_message, 58);
      draw_text(font_, 456, 430, kColorMuted, kTextScaleSmall, status.c_str());
    }
    return;
  }

  const std::string selected_label = truncate_label(save->display_name, 36);
  draw_text(font_, 456, 110, kColorMuted, kTextScaleSmall, selected_label.c_str());

  const std::vector<BackupEntry> entries = build_backup_menu(
      {"2026-05-21 16-14.zip", "2026-05-20 22-31.zip"}, {"2026-05-19 09-05.zip"});

  int y = 136;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const bool selected = i == 1;
    const unsigned int bg = selected ? kColorAccentSoft : RGBA8(255, 255, 255, 18);
    vita2d_draw_rectangle(456, y, 460, 38, bg);
    if (selected) {
      vita2d_draw_rectangle(456, y, 4, 38, kColorAccent);
    }

    const std::string label = entries[i].display_name();
    draw_text(font_, 470, y + 25, selected ? kColorText : kColorMuted, kTextScaleSmall,
              label.c_str());
    y += 46;
  }

  const std::string status =
      status_message.empty() ? "O creates a local timestamped ZIP snapshot."
                             : truncate_label(status_message, 58);
  draw_text(font_, 456, 430, kColorMuted, kTextScaleSmall, status.c_str());
}

void Ui::draw_footer() const {
  vita2d_draw_rectangle(0, 508, 960, 36, RGBA8(3, 7, 18, 230));
  draw_text(font_, 18, 532, kColorMuted, kTextScaleSmall,
            "D-Pad Select Save    O New Backup    Square Restore    START Exit");
}

} // namespace vsm::vita
