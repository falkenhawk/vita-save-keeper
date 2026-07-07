#pragma once

#include "core/BackupList.hpp"
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
  // One row per snapshot (index 0 = "New Backup"), merged from the local and Drive lists by the
  // App; presence on each side renders as pills instead of text prefixes.
  const std::vector<BackupRow> *backup_rows{};
  std::size_t selected_backup{};
  bool restore_confirmation_pending{};
  bool delete_confirmation_pending{};
  bool delete_scope_prompt_pending{};
  bool sync_all_confirmation_pending{};
  bool duplicate_backup_confirmation_pending{};
  // 0 when idle; fraction of the active hold gesture (Select = batch, Square = label) completed,
  // drawn as a gauge under the status line while the hold hint is showing.
  float hold_gauge_fraction{};
  bool google_connected{};
  bool drive_synced{};
  bool google_auth_pending{};
  // Live internet state; signed-in without a connection renders as "Google Drive offline".
  bool network_connected{true};
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

// Outcome of the modal IME dialog; Failed means the dialog could not open at all.
enum class TextInputResult { Accepted, Canceled, Failed };

class Ui {
public:
  bool initialize();
  void shutdown();
  void draw(const UiState &state);
  // Blocking system-keyboard prompt rendered over a dimmed snapshot of the last frame, following
  // the draw_busy precedent. Waits for pad release before returning so the closing press cannot
  // leak into the main loop as a fresh button edge.
  TextInputResult prompt_text_input(const char *title, const std::string &initial_text,
                                    std::size_t max_length, std::string *out_text);
  // "prefix + name + suffix" fitted to the status line by ellipsizing only the name, so fixed
  // instruction text always survives. Measures the whole composed string per step: a CJK name
  // routes the entire line to the wider PGF fallback font, so the fixed parts cannot be measured
  // separately with Latin-font metrics.
  std::string compose_status_with_name(const std::string &prefix, const std::string &name,
                                       const std::string &suffix) const;
  // Full-screen modal frame for blocking work; safe to call from transfer callbacks because all
  // network requests run on the UI thread. total <= 0 draws an indeterminate sweep.
  void draw_busy(const std::string &label, long long done, long long total);
  // Batch context for draw_busy: while set, the modal's title and bar track overall games
  // progress, any transfer progress passed to draw_busy shows as a percent line instead of a
  // second bar, and a "hold to cancel" hint appears.
  void set_batch_progress(std::string label, std::size_t done_games, std::size_t total_games,
                          bool cancel_is_circle);
  void clear_batch_progress();

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
  // Presence glyphs pre-rendered as antialiased PNGs (vita2d circles are unantialiased triangle
  // fans, which looked jagged on hardware); null falls back to the primitive-drawn shapes.
  vita2d_texture *cloud_synced_tex_{};
  vita2d_texture *cloud_drive_only_tex_{};
  std::string batch_label_;
  std::size_t batch_done_{};
  std::size_t batch_total_{};
  bool batch_cancel_is_circle_{true};
  bool batch_active_{};
  // Snapshot of the last frame's state so busy frames can repaint the UI, dimmed, behind the
  // progress modal. The embedded pointers reference App members that outlive any operation.
  UiState last_state_;
  bool has_last_state_{};
  std::size_t title_top_row_{};
  std::size_t backup_top_row_{};
  // Marquee state for the focused backup row: which row is scrolling and how many frames it has
  // been focused, so the scroll restarts from the left whenever the selection moves.
  std::size_t marquee_entry_{};
  unsigned int marquee_frame_{};
  unsigned int frame_counter_{};
  std::map<std::string, vita2d_texture *> icon_cache_;
};

} // namespace vsm::vita
