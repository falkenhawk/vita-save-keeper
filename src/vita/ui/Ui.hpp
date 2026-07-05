#pragma once

#include "core/SaveCategory.hpp"
#include "core/SaveRecord.hpp"
#include "core/SaveScanner.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct vita2d_pgf;
struct vita2d_font;
struct vita2d_texture;

namespace vsm::vita {

enum class StatusKind { Info, Success, Error };

// Grid width of the save panel; D-pad up/down moves by one full row, so the input handler in App
// must use the same value the renderer lays tiles out with.
constexpr int kSaveGridColumns = 5;

// One snapshot of everything the frame needs. Passing a struct keeps the App/Ui boundary explicit
// and stops the draw call signature from growing a parameter per feature.
struct UiState {
  const std::vector<SaveRecord> *saves{};
  // Indices into saves for the active category tab; selected_save indexes this list.
  const std::vector<std::size_t> *visible_saves{};
  SaveCategory active_category{SaveCategory::VitaGame};
  SaveSortMode sort_mode{SaveSortMode::Name};
  std::array<std::size_t, kSaveCategoryCount> category_counts{};
  std::size_t selected_save{};
  std::vector<std::string> remote_backups;
  const std::vector<std::string> *local_backups{};
  std::size_t selected_backup{};
  bool restore_confirmation_pending{};
  bool delete_confirmation_pending{};
  bool google_connected{};
  bool drive_synced{};
  bool google_auth_pending{};
  // Which physical button the system treats as "enter"; western consoles use Cross, Japanese
  // consoles use Circle. Primary/cancel symbols in the footer follow it.
  bool enter_is_cross{true};
  std::string google_verification_url;
  std::string google_user_code;
  int auth_seconds_left{};
  std::string status_message;
  StatusKind status_kind{StatusKind::Info};
};

// One vita2d_font instance per text size: the library caches each glyph bitmap at whatever size
// it was first drawn with and bilinearly rescales it for any other size, which is what blurred
// small text on hardware. Separate per-size instances keep every glyph pixel-exact.
struct FontSet {
  static constexpr unsigned int kMaxSize = 64;
  vita2d_font *by_size[kMaxSize] = {};
  vita2d_pgf *fallback = nullptr;
};

class Ui {
public:
  bool initialize();
  void shutdown();
  void draw(const UiState &state);
  // Full-screen modal frame for blocking work; safe to call from transfer callbacks because all
  // network requests run on the UI thread. total <= 0 draws an indeterminate sweep.
  void draw_busy(const std::string &label, long long done, long long total);

private:
  void draw_header(const UiState &state);
  void draw_title_grid(const UiState &state);
  void draw_backup_panel(const UiState &state);
  void draw_rstick_hint(int cx, int cy);
  void draw_google_auth_panel(const UiState &state);
  void draw_status_line(const UiState &state);
  void draw_footer(const UiState &state);
  int measure_text(unsigned int size, const char *text) const;
  std::string fit_text(unsigned int size, const std::string &text, int max_width) const;
  vita2d_texture *load_icon_texture(const std::string &path);

  FontSet fonts_;
  // Snapshot of the last frame's state so busy frames can repaint the UI, dimmed, behind the
  // progress modal. The embedded pointers reference App members that outlive any operation.
  UiState last_state_;
  bool has_last_state_{};
  std::size_t title_top_row_{};
  std::size_t backup_top_row_{};
  unsigned int frame_counter_{};
  std::map<std::string, vita2d_texture *> icon_cache_;
};

} // namespace vsm::vita
