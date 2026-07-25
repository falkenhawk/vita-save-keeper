#pragma once

#include "core/BackupList.hpp"
#include "core/SaveCategory.hpp"
#include "core/SaveRecord.hpp"
#include "core/SaveScanner.hpp"
#include "core/SaveSlotMetadata.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct vita2d_pgf;
struct vita2d_font;
struct vita2d_texture;

namespace vsm::vita {

enum class StatusKind { Info, Success, Error };

// Modal shown when connecting fails because the OAuth client credentials are absent, unusable,
// or rejected by Google's device endpoint.
enum class GoogleSetupPrompt { None, MissingFile, InvalidFile, RejectedByGoogle };

// Step-by-step setup guide; the setup prompt renders it as a QR code next to its textual form.
inline constexpr const char *kGoogleSetupGuideUrl =
    "https://github.com/falkenhawk/vita-save-keeper/blob/master/docs/google-drive-setup.md";

struct SlotDetailsState {
  bool open{};
  // Mirrors UiState::google_connected (this screen draws from its own state): without a Google
  // sign-in the card/cloud distinction does not exist, so the presence glyph stays hidden.
  bool google_connected{};
  std::string game_title;
  std::string snapshot_name;
  SaveMetadata metadata;
  std::size_t selected_slot{};
  int details_scroll{};
  std::string unavailable_message;
  std::string warning_message;
  // One size per view, computed on demand when it opens so the grid never pays for it.
  // save_bytes is the live save's on-disk footprint ("New Backup" row); archive_bytes is a
  // snapshot's ZIP file size (local stat, or the Drive-reported size for a Cloud-only copy).
  // The ZIP stores entries uncompressed, so a separate content size would differ from the file
  // size only by header overhead. Each *_known is false when unreadable or not applicable.
  std::uint64_t save_bytes{};
  bool save_bytes_known{};
  std::uint64_t archive_bytes{};
  bool archive_bytes_known{};
  // Where the inspected snapshot lives, mirroring the overview's cloud glyph in the header
  // corner. Both false for the live save, which is not a snapshot.
  bool snapshot_on_card{};
  bool snapshot_in_cloud{};
  // Describe the inspected save itself (not the snapshot row), so the live row's footer can offer
  // the batch-skip toggle only for homebrew entries and label it by the current state.
  bool entry_is_homebrew{};
  bool entry_skipped{};
  // True when details was opened with the "Data folders" row focused. The screen still shows the
  // live save (that row is not a snapshot), but the footer offers the picker instead of a backup.
  bool data_folders_row{};
  // The entry's extra ux0:data folders (SaveRecord.extra_paths, path only). A data folder has no
  // sdslot metadata, so when this is non-empty the renderer lists these alongside the slot table
  // rather than falling back to "no readable slot metadata". Empty for every entry with none.
  std::vector<std::string> extra_paths;
};

// Modal picker for a homebrew entry's data folders. Confined to "ux0:data" and never climbs above
// it, so an included folder always stays on the ux0 filesystem. Rows are the child directories of
// current_path; each row's size is filled in lazily (debounced) after the selection settles, so
// browsing a folder full of huge trees does not stat-walk every one at once.
struct DirectoryBrowserState {
  bool open{};
  // The entry whose backups the picked folders join, captured when the browser opens so a later
  // re-sort or refresh cannot retarget an in-flight pick.
  std::string entry_id;
  std::string entry_name;
  // Opens at the best guess for this entry (see App::browser_start_path). start_path remembers it:
  // Circle climbs one level at a time while strictly inside it and closes once back at (or
  // outside) it - going higher than the start is the ".." row's job.
  std::string current_path;
  std::string start_path;
  struct Row {
    std::string name;
    // The ".." row shown below the root: Cross goes up a level. Carries no size and no state.
    bool parent_link{};
    bool size_known{};
    std::uint64_t size_bytes{};
    // True while the incremental stat-walk is measuring this row; the size column spins.
    bool sizing{};
    // Included in the backups of the entry this browser was opened from; Square excludes it again.
    bool tracked_here{};
    // Inside a folder this entry already includes (covered_by names it). Backed up through the
    // parent, so it cannot be included or excluded on its own.
    std::string covered_by;
    // Backed up by some *other* entry, directly or through a parent. Shown as taken and left
    // alone: one folder belongs to one app, and excluding it from here would silently edit an
    // entry the user is not looking at.
    bool tracked_elsewhere{};
  };
  std::vector<Row> rows;
  std::size_t selected{};
  // How many folders this entry currently includes, across every directory - drives the L/R
  // jump-to-included hint.
  std::size_t included_count{};
  // Set when the focused folder is over the large-folder threshold and a first Square press asked
  // for confirmation; any other input clears it, a second Square press then includes it anyway.
  bool large_confirm_pending{};
  // True when the browser was opened from Save Details, so closing it goes back there rather than
  // dropping the user out to the grid.
  bool return_to_details{};
};

// Grid width of the save panel; D-pad up/down moves by one full row, so the input handler in App
// must use the same value the renderer lays tiles out with.
constexpr int kSaveGridColumns = 5;

// One snapshot of everything the frame needs. Passing a struct keeps the App/Ui boundary explicit
// and stops the draw call signature from growing a parameter per feature.
struct UiState {
  const std::vector<SaveRecord> *saves{};
  // Indices into saves for the active category tab; selected_save indexes this list.
  const std::vector<std::size_t> *visible_saves{};
  // Save ids left out of the hold-Select batch; their grid tiles carry a small "skip" tag. Grid
  // order is unaffected - skipping changes what the sweep touches, nothing else.
  const std::set<std::string> *skipped_ids{};
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
  // Whether the pending batch would upload; without Drive the confirm hint drops "& Upload".
  bool sync_all_will_upload{};
  bool duplicate_backup_confirmation_pending{};
  // 0 when idle; fraction of the active hold gesture (Select = batch, Square = label, Triangle =
  // Google action) completed,
  // drawn as a gauge under the status line while the hold hint is showing.
  float hold_gauge_fraction{};
  bool google_connected{};
  bool drive_synced{};
  bool google_auth_pending{};
  GoogleSetupPrompt google_setup_prompt{GoogleSetupPrompt::None};
  // Live internet state; signed-in without a connection renders as "Google Drive offline".
  bool network_connected{true};
  // Which physical button the system treats as "enter"; western consoles use Cross, Japanese
  // consoles use Circle. Primary/cancel symbols in the footer follow it.
  bool enter_is_cross{true};
  const SlotDetailsState *slot_details{};
  const DirectoryBrowserState *directory_browser{};
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
  int details_max_scroll(const SlotDetailsState &state) const;
  // Blocking system-keyboard prompt rendered over a dimmed snapshot of the last frame, following
  // the draw_busy precedent. Waits for pad release before returning so the closing press cannot
  // leak into the main loop as a fresh button edge.
  TextInputResult prompt_text_input(const char *title, const std::string &initial_text,
                                    std::size_t max_length, std::string *out_text);
  // 'prefix "name" suffix' fitted by ellipsizing only the quoted name, so the fixed instruction
  // text always survives. Measures the whole composed string per step: a CJK name routes the
  // entire line to the wider PGF fallback font, so the fixed parts cannot be measured separately
  // with Latin-font metrics. The _status_ variant targets the status line; the _modal_ variant
  // targets the busy modal's title (wider font, box width), so a long name never eats a suffix
  // like " Cloud backup" or the closing quote.
  // max_width 0 uses the overview status line's budget; the details footer line passes a wider
  // one, since its status shares the bar with fewer, shorter hints.
  std::string compose_status_with_name(const std::string &prefix, const std::string &name,
                                       const std::string &suffix, int max_width = 0) const;
  std::string compose_modal_label(const std::string &prefix, const std::string &name,
                                  const std::string &suffix) const;
  // Human-readable byte size, shared with the details/browser panes so a status line composed in
  // App (the large-folder confirmation) formats sizes identically to the on-screen size column.
  std::string format_size(std::uint64_t bytes) const;
  // Full-screen modal frame for blocking work; safe to call from transfer callbacks because all
  // network requests run on the UI thread. total <= 0 draws an indeterminate sweep.
  // context_above draws a muted line above the modal (the reason the operation is running);
  // cancel_hint draws a Square-button hint below it (an escape hatch). Both optional.
  void draw_busy(const std::string &label, long long done, long long total,
                 const char *context_above = nullptr, const char *cancel_hint = nullptr);
  // Batch context for draw_busy: while set, the modal's title and bar track overall games
  // progress, any transfer progress passed to draw_busy shows as a percent line instead of a
  // second bar, and a "hold to cancel" hint appears.
  // action is the verb ("Uploading"), game is the title. The modal renders "action game (N/M)"
  // and, when it does not fit, ellipsizes only the game so the (N/M) counter always shows.
  void set_batch_progress(std::string action, std::string game, std::size_t done_games,
                          std::size_t total_games, bool cancel_is_circle);
  void clear_batch_progress();

private:
  // The previous frame, dimmed, as a modal's background; null-safe against the details view's
  // partial UiState. Shared by draw_busy and prompt_text_input.
  void draw_modal_backdrop();
  // The card/Cloud/both cloud glyph shared by the backup rows and the details header; textures
  // with a primitive fallback. Draws nothing when both flags are false. background is the surface
  // the glyph sits on - the fallback cuts its check/arrow out in that colour, and the rows and the
  // header are different tones.
  void draw_presence_glyph(int x, int y, bool on_card, bool in_cloud, unsigned int background);
  void draw_header(const UiState &state);
  void draw_title_grid(const UiState &state);
  void draw_backup_panel(const UiState &state);
  void draw_rstick_hint(int cx, int cy);
  void draw_google_auth_panel(const UiState &state);
  void draw_status_line(const UiState &state);
  void draw_footer(const UiState &state);
  void draw_google_setup_prompt(const UiState &state);
  // status_message/status_kind mirror the overview's status line in this screen's footer, so the
  // carried-over actions (transfer, backup/restore, label) and their confirmation prompts stay
  // visible here. While a confirmation is pending the prompt renders in the accent color and the
  // footer reduces to confirm/cancel.
  void draw_slot_details(const SlotDetailsState &state, bool enter_is_cross,
                         const std::string &status_message, StatusKind status_kind,
                         bool restore_confirmation_pending,
                         bool duplicate_backup_confirmation_pending);
  // The directory-picker modal; takes the shared status line the same way draw_slot_details does so
  // the track action's feedback (already tracked, too large, large-folder confirmation) shows here.
  void draw_directory_browser(const DirectoryBrowserState &state, bool enter_is_cross,
                              const std::string &status_message, StatusKind status_kind);
  int measure_text(unsigned int size, const char *text) const;
  std::string fit_text(unsigned int size, const std::string &text, int max_width) const;
  // Ellipsizes from the left ("...ata/retroarch") so a long path keeps its trailing leaf visible,
  // the mirror of fit_text which keeps the head.
  std::string fit_text_left(unsigned int size, const std::string &text, int max_width) const;
  std::vector<std::string> wrap_text(unsigned int size, const std::string &text,
                                     int max_width) const;
  std::string fit_quoted_name(const std::string &prefix, const std::string &name,
                              const std::string &suffix, unsigned int size, int max_width,
                              bool quote = true) const;
  vita2d_texture *load_icon_texture(const std::string &path);

  FontSet fonts_;
  // Presence glyphs pre-rendered as antialiased PNGs (vita2d circles are unantialiased triangle
  // fans, which looked jagged on hardware); null falls back to the primitive-drawn shapes.
  vita2d_texture *cloud_synced_tex_{};
  vita2d_texture *cloud_drive_only_tex_{};
  vita2d_texture *cloud_local_only_tex_{};
  std::string batch_action_;
  std::string batch_game_;
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
  std::size_t browser_top_row_{};
  // Picker glyphs, loaded with the cloud set; primitive fallbacks cover a failed load.
  vita2d_texture *folder_edit_tex_{};
  vita2d_texture *mark_available_tex_{};
  vita2d_texture *mark_included_tex_{};
  vita2d_texture *mark_covered_tex_{};
  vita2d_texture *mark_inuse_tex_{};
  // Marquee state for the focused backup row: which row is scrolling and how many frames it has
  // been focused, so the scroll restarts from the left whenever the selection moves.
  std::size_t marquee_entry_{};
  unsigned int marquee_frame_{};
  unsigned int frame_counter_{};
  std::map<std::string, vita2d_texture *> icon_cache_;
};

} // namespace vsm::vita
