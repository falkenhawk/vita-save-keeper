#pragma once

#include "core/BackupList.hpp"
#include "core/GoogleAuth.hpp"
#include "core/GoogleConfig.hpp"
#include "core/GoogleDrive.hpp"
#include "core/SaveCategory.hpp"
#include "core/SaveRecord.hpp"
#include "core/SaveScanner.hpp"
#include "core/SaveSlotMetadata.hpp"
#include "core/SaveTimeCache.hpp"
#include "core/SyncPlan.hpp"
#include "core/TrackedFolders.hpp"
#include "vita/net/HttpClient.hpp"
#include "vita/ui/Ui.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <dirent.h>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace vsm::vita {

struct RemoteBackup {
  std::string name;
  std::string file_id;
  // From the Drive listing; 0 when Drive did not report a size. Lets the details view show a
  // Cloud-only backup's ZIP size without downloading it.
  long long size_bytes{};
};

struct LocalSnapshotResult {
  // A snapshot may succeed even when its optional JSON companion does not. Callers use this
  // warning to tell the user without pretending the ZIP backup failed.
  bool ok{};
  bool reused{};
  bool metadata_warning{};
  std::string archive_name;
  std::string error;
};

struct BackupUploadResult {
  // Drive follows the same rule: the ZIP is the backup; metadata is helpful but optional.
  bool ok{};
  bool metadata_warning{};
};

class App {
public:
  int run();

private:
  void refresh_local_backups();
  void move_selected_save(int delta);
  // Up/down in the grid wrap within the column: off the bottom back to its top, off the top to
  // its bottom-most occupied cell - honest about a partial last row, unlike a +-columns modulo
  // jump that landed columns away.
  void move_selected_save_vertical(int direction);
  void move_selected_save_to(std::size_t target);
  void move_selected_backup(int delta);
  void move_selected_category(int delta);
  void cycle_sort_mode();
  void apply_sort_and_rebuild();
  std::map<std::string, std::string> newest_backup_by_folder() const;
  void load_settings();
  void save_settings();
  void rebuild_visible_saves();
  // Adds the focused save to the read queue when its time still needs a mount. Navigating never
  // waits on that read, so passing over several encrypted saves simply queues each one.
  void queue_selected_save_time_read();
  // Reads one queued save time, focused save first. Returns false when nothing was read, either
  // because the queue is empty or because a button arrived and navigation takes priority.
  bool drain_pending_time_read();
  // Writes the index once the reads are done and the pause has run long, keeping its cost off the
  // frame that finished the last read.
  void write_save_index_when_idle();
  // The mount worker: one thread that owns every PFS mount so the main thread never blocks on
  // one and mounts never mix threads (the prime suspect for the historical mount-slot leak).
  void start_mount_worker();
  void stop_mount_worker();
  static int mount_worker_entry(unsigned int args, void *argp);
  // Blocking resolve through the worker, same semantics the old free function had; callers that
  // hold a modal anyway (backups, details, the batch read) stay unchanged. Falls back to an
  // inline resolve when the worker failed to start.
  SaveMetadata resolve_live_save_metadata(const std::string &save_path,
                                          const SaveDateTime &backup_clock,
                                          bool allow_pfs_mount, bool bridge_available);
  // Hands one queued save to the worker (mount + metadata + post-mount fingerprint) and returns
  // immediately; complete_async_read applies the result on the main thread when it lands.
  void submit_async_save_time_read(const SaveRecord &save);
  void complete_async_read(bool wait);
  // Re-locates id within visible_saves_, setting selected_save_ to its index (0 when id is empty
  // or absent) and syncing the current tab's remembered position to match. Used after a re-sort
  // reorders saves_ and after attaching a folder; callers call rebuild_visible_saves() first.
  void refocus_selection_by_id(const std::string &id);
  // Reads backup-settings.json once at startup (bounded, capped). A present-but-unreadable or
  // unparseable file sets tracked_config_load_failed_ and leaves the config empty, so it is never
  // overwritten later.
  void load_tracked_folders();
  // Writes the user config to backup-settings.json (atomic). False (with a status set) on failure.
  bool save_data_folders_config();
  // The base entry for id, or null when there is none or none of its folders exist here.
  const TrackedFolderEntry *applicable_base_entry(const std::string &id) const;
  // A held L in the picker: puts the entry back on its shipped base paths. The snap-back in
  // update_data_folder_override then drops the override, so future base updates apply again.
  void browser_restore_defaults();
  // Routes one UI change through update_data_folder_override + persistence + re-apply, so an entry
  // matching the shipped base again drops its override and follows future base updates. Stamps
  // modified; the Drive copy catches up at the next boot/refresh reconcile.
  bool set_entry_data_folders(const std::string &id, const std::string &title,
                              std::vector<TrackedPath> new_paths);
  // Reconciles backup-settings.json with its Drive copy (PSV Saves root). Runs after every
  // successful Drive index sync - boot, connect, and the manual refresh. Last writer wins; when
  // the local side loses a two-sided conflict its version is kept as backup-settings.conflict.json
  // (a losing remote is recoverable from Drive's revision history instead).
  void sync_backup_settings();
  // The reconcile, only when local changes await: rides a backup upload (single or once per batch)
  // so changes reach Drive at the next natural network moment.
  void sync_backup_settings_if_dirty();
  // Uploads the local file over the Drive copy (create or update) and records the agreement stamp.
  bool upload_backup_settings();
  // Attaches each config entry's extra data folders to its app's record in saves_, synthesizes
  // records for configured apps the scan found no savedata folder for, and resolves the time of
  // every entry that ends up with extras. Idempotent, and it *assigns* rather than appends, so a
  // repeat call after an exclude also drops what an earlier call attached. Must run after a scan
  // populates saves_ and before the sort/rebuild; callers re-sort afterwards. resolve_times=false
  // skips the per-entry mtime walks - the picker toggles pass it and settle times once on close.
  void apply_tracked_folders(bool resolve_times = true);
  // Returns false if the user canceled (Square) mid-read, so the caller can keep the name order.
  bool resolve_all_save_times();
  bool resolve_save_time(SaveRecord *save);
  // Fills save times from save_index_ for every mount-requiring save whose folder fingerprint
  // still matches a resolved entry, clearing save_time_requires_mount so those saves skip the
  // mount entirely.
  void apply_cached_save_times();
  void flush_save_index();
  // Rebuilds the index from the scanned records against the given app-database stamp, writing
  // only when something actually changed. Applied cached times are read from the records, so
  // apply_cached_save_times runs first; the rebuild's own carry-forward would cover the other
  // order too.
  void rebuild_save_index(long long app_db_mtime, long long app_db_size);
  // After a restore rewrote the live folder: clear the entry's cached time back to never-resolved
  // (titles and the old fingerprint stay) and re-derive the record's time state so the grid never
  // keeps showing the pre-restore save time.
  void invalidate_save_time(const SaveRecord &restored);
  std::size_t category_count(SaveCategory category) const;
  const SaveRecord *selected_save_record() const;
  void cancel_restore_confirmation();
  void cancel_delete_confirmation();
  // Where the folder picker opens for this entry: beside a folder it already has, else the closest
  // ux0:data name match for its title, else ux0:data itself. app.db stores no data-folder path, so
  // this is a guess and only ever picks a starting point.
  std::string browser_start_path(const SaveRecord &save) const;
  // Directory picker for the focused homebrew entry's data folders. Confined to ux0:data; Square
  // includes or excludes the focused folder and the browser stays open, so several can be set in
  // one visit. from_details makes closing return to Save Details instead of the grid.
  //
  // Nothing here creates or deletes a directory - it only records which existing folders the entry
  // backs up - so the wording stays "include"/"exclude" throughout, in the UI and in these names.
  void open_directory_browser(bool from_details);
  // Applies everything the visit changed in one go: re-sort, refocus the entry, refresh its backup
  // lists, and reopen Save Details when that is where the browser came from.
  void close_directory_browser();
  // Re-lists current_path and re-tags the rows (sizes come back from the per-visit cache, so a
  // toggle never wipes what was already measured). keep_selection holds the cursor in place, which
  // is what a toggle wants; a drill or climb passes false and starts at the top.
  void reload_browser_rows(bool keep_selection = false);
  // Climbs to current_path's parent and lands the cursor on the folder just left.
  void browser_go_up();
  // L/R: focus the previous/next included folder, switching directories when it lives elsewhere.
  void browser_jump_included(int delta);
  // Square: includes the focused folder in this entry's backups, or excludes it again if it is
  // already one of theirs. A folder another entry backs up, or one nested either way with an
  // already-included folder, is refused - overlapping trees would back up twice and restore twice.
  void browser_toggle_selected();
  void browser_exclude_selected(const std::string &full_path);
  void schedule_browser_size_resolve();
  // The focused row's size is measured incrementally, a bounded slice of stat calls per frame, so
  // a huge tree (VitaDB's thousands of thumbnails, say) never freezes input while it is sized.
  void start_browser_size_walk();
  void abort_browser_size_walk();
  void advance_browser_size_walk();
  void fill_browser_row_size(const std::string &path, std::uint64_t bytes);
  // Recomputes every row's spinner flag from the active walk and the hover queue.
  void refresh_browser_sizing_marks();
  void handle_action_button();
  void create_new_backup();
  // busy_label, when set, names the modal that animates byte progress while the save is hashed
  // and zipped; during a batch the title is owned by the batch context and only the byte
  // progress shows. nullptr keeps the operation fully silent.
  LocalSnapshotResult create_local_snapshot(const SaveRecord &save, const std::string &suffix,
                                            const char *busy_label, bool force_new = false);
  void handle_restore();
  void handle_delete_button();
  void load_google_token_cache();
  bool load_google_credentials();
  void handle_google_button();
  void begin_google_auth();
  void cancel_google_auth();
  void update_google_auth();
  void poll_google_token();
  bool save_google_token_cache();
  bool refresh_google_access_token();
  bool ensure_google_access_token();
  HttpResponse drive_request(const std::function<HttpResponse(const std::string &)> &send);
  std::string find_drive_folder(const std::string &folder_name, const std::string &parent_id);
  std::string find_or_create_drive_folder(const std::string &folder_name,
                                          const std::string &parent_id);
  std::string resolved_drive_folder_name(const std::string &save_id) const;
  bool sync_drive_index();
  void remove_drive_folder_if_empty(const std::string &folder_name);
  void refresh_remote_backups_view();
  std::vector<std::string> remote_backup_names() const;
  void rebuild_backup_rows();
  std::size_t backup_count() const;
  const BackupRow *selected_backup_row() const;
  // True when the focused row is the homebrew entry's "Savedata Paths" action row. It is a sentinel,
  // so selected_backup_row() reports it as no row at all; this is how the action button tells the
  // two "no row" cases apart.
  bool data_folders_row_focused() const;
  bool new_backup_row_focused() const;
  // Index of the "New Backup" row - the default focus whenever the backup list resets. Not 0 on
  // homebrew entries, where the "Savedata Paths" row sits above it.
  std::size_t default_backup_row() const;
  std::string selected_backup_name() const;
  void focus_backup_row_by_identity(const std::string &backup_name);
  std::string remote_file_id_for(const std::string &remote_name) const;
  long long remote_size_for(const std::string &remote_name) const;
  void perform_scoped_delete(bool delete_local, bool delete_remote);
  void begin_label_edit();
  bool rename_remote_backup(const SaveRecord &save, const std::string &remote_name,
                            const std::string &new_name);
  void handle_transfer_button();
  bool download_remote_backup_to_card(const SaveRecord &save, const BackupRow &row,
                                      std::string *error);
  BackupUploadResult upload_local_backup(const SaveRecord &save,
                                         const std::string &backup_name);
  BackupUploadResult upload_local_backup_impl(const SaveRecord &save,
                                              const std::string &backup_name);
  // Read a healthy local JSON companion, or rebuild it from sdslot.dat inside an older ZIP.
  SaveMetadataJsonResult ensure_local_backup_metadata(const SaveRecord &save,
                                                       const std::string &backup_name);
  // Prefer the stable ZIP file-id link. The name lookup keeps early/hand-made companions usable.
  DriveFile find_remote_sidecar(const std::string &folder_id,
                                const std::string &archive_file_id,
                                const std::string &archive_name);
  SaveMetadataJsonResult download_remote_backup_metadata(const SaveRecord &save,
                                                          const std::string &archive_name,
                                                          const std::string &archive_file_id);
  void open_save_details();
  void request_save_details();
  void repair_remote_backup_metadata(const SaveRecord &save, const BackupRow &row,
                                     bool replace_unusable = false);
  bool remote_backup_exists(const std::string &save_id, const std::string &backup_name) const;
  void begin_sync_all();
  void run_sync_all();
  void cancel_sync_all_confirmation();
  // The confirmation window doubles as a picker: checkboxes appear on the grid tiles, the d-pad
  // browses, R checks the focused save in or out, and a held L flips everything at once. Choices
  // are transient until the run confirms, when this tab's picks fold into skipped_ids - so cancel
  // discards them, and other tabs' skips are never touched.
  std::size_t batch_selected_count() const;
  void update_sync_all_confirm_status();
  void batch_toggle_focused();
  void batch_toggle_all();
  void cancel_duplicate_backup_confirmation();
  bool poll_batch_cancel();
  void set_status(StatusKind kind, std::string message);
  void clear_status();
  // compose_status_with_name for whichever screen will render it: the details footer line fits
  // far more than the overview's 380px status column, so names truncate later there.
  std::string status_with_name(const std::string &prefix, const std::string &name,
                               const std::string &suffix) const;

  GoogleClientCredentials google_credentials_;
  GoogleTokenCache google_token_cache_;
  DeviceCodeResponse device_code_;
  std::vector<SaveRecord> saves_;
  // Indices into saves_ for the active category tab; selected_save_ indexes this list.
  std::vector<std::size_t> visible_saves_;
  SaveCategory category_{SaveCategory::VitaGame};
  SaveSortMode sort_mode_{SaveSortMode::Name};
  bool cleaned_empty_backup_folders_{};
  // Which save was focused in each category tab, so L/R returns to where the user left off.
  std::array<std::size_t, kSaveCategoryCount> category_selection_{};
  std::vector<std::string> local_backups_;
  // remote_backups_ is the per-save view derived from drive_index_, which holds every backup in
  // Drive keyed by save folder name. The index is filled by one paginated sync instead of a
  // network round-trip per selected game.
  std::vector<RemoteBackup> remote_backups_;
  // One row per snapshot, local and Drive copies merged by timestamp identity; index 0 is the
  // "New Backup" sentinel and selected_backup_ indexes this list directly.
  std::vector<BackupRow> backup_rows_;
  std::map<std::string, std::vector<RemoteBackup>> drive_index_;
  std::unordered_map<std::string, std::string> drive_folder_ids_;
  std::string drive_root_folder_id_;
  bool drive_synced_{};
  std::size_t selected_save_{};
  // Index into the backup menu: 0 is the "New Backup" entry, backups follow at 1..backup_count().
  std::size_t selected_backup_{};
  bool restore_confirmation_pending_{};
  bool delete_confirmation_pending_{};
  // A snapshot living on card and Drive needs a scope choice instead of a plain second press:
  // Start deletes both sides, Triangle only the Drive copy, Square only the card copy.
  bool delete_scope_prompt_pending_{};
  // Second-press force for a manual backup whose content already exists in a local archive.
  bool duplicate_backup_confirmation_pending_{};
  bool sync_all_confirmation_pending_{};
  // Baked when the batch confirmation opens, so the footer hint matches the prompt text even if
  // the network state changes while the prompt is up.
  bool sync_all_will_upload_{};
  // Ids of this tab's entries checked OUT of the pending sweep, seeded from skipped_ids when the
  // window opens and only ever holding visible ids. Discarded on cancel; folded back on confirm.
  std::set<std::string> batch_deselected_;
  // Set while run_sync_all is executing; the HTTP cancel check polls the pad only then, so a
  // held cancel button can abort an in-flight upload without affecting single-file transfers.
  bool batch_running_{};
  bool batch_cancel_requested_{};
  // Why the last credentials load failed, and the modal (if any) currently explaining it. Only
  // the user-initiated connect opens the modal; background token refreshes never do.
  GoogleSetupPrompt google_credentials_error_{GoogleSetupPrompt::None};
  GoogleSetupPrompt google_setup_prompt_{GoogleSetupPrompt::None};
  bool google_connected_{};
  // Live internet state, polled once per second; a stored sign-in without a connection shows as
  // "offline" and turns the batch into a backups-only run.
  bool network_connected_{};
  // Set when sign-in completes so the first Drive listing runs one frame later, after the
  // "connected" state has been drawn, instead of freezing the sign-in screen.
  bool pending_remote_refresh_{};
  // True only when both mount-bridge modules loaded at startup. Gates the kernel syscall so slot
  // resolution degrades to the AppMgr mount (and then save-file times) when the bridge is absent.
  bool mount_bridge_ready_{};
  // Merged save index (times and titles) keyed by save id. Times and sfo-derived titles are
  // trusted while each save folder's fingerprint is unchanged, app-database titles while the
  // database stamp is. Loaded once at startup; flushed after each resolve batch and rebuild.
  SaveIndex save_index_;
  bool save_index_dirty_{};
  // Save ids whose mount-only time is still unread, in the order they were focused; reads take
  // the focused save first and then the newest entries, since those sit next to the cursor. Each
  // is read one at a time (mounts must stay sequential) while the pad is idle, so scrolling past
  // a row of encrypted saves stays smooth and each keeps its spinner until its turn comes.
  std::vector<std::string> pending_time_reads_;
  // Frames since the last button. The drain waits for a short idle gap so a read never starts
  // under a press that is about to move the selection.
  int input_idle_frames_{};
  // Buttons seen in the pad's sample history while the index write blocked the frame, replayed on
  // the next frame: the loop peeks one instantaneous sample, so a tap contained inside the write
  // is otherwise invisible and the navigation it asked for is lost.
  unsigned int deferred_buttons_{};
  // The single mount-work slot. The worker only touches it in state 1 and the main thread only in
  // states 0 and 2, so the atomic is the entire handshake; one request is in flight at a time.
  struct MountWork {
    std::string save_path;
    SaveDateTime backup_clock;
    bool allow_pfs_mount{};
    bool want_fingerprint{};
    // Non-empty for an entry with extra paths: the worker resolves the newest time across these
    // directories and files instead of mounting anything, and no fingerprint is taken - the
    // result must stay out of the save index, whose fingerprints cover only savedata folders.
    std::vector<std::string> tracked_paths;
    // Set for queued reads, which return to the main loop instead of waiting: the id the result
    // belongs to, and whether a restore made it stale before it landed.
    bool async{};
    bool discard{};
    std::string async_save_id;
    SaveMetadata metadata;
    SaveFingerprint fingerprint;
  };
  MountWork mount_work_;
  // 0 idle, 1 submitted, 2 done.
  std::atomic<int> mount_work_state_{0};
  std::atomic<bool> mount_worker_stop_{false};
  int mount_worker_thread_{-1};
  int mount_worker_wake_{-1};
  // Extra data folders per homebrew entry plus the batch-skip list, loaded once at startup and
  // applied to saves_ after each scan.
  TrackedFoldersConfig tracked_config_;
  // Folder sets shipped in the VPK (app0:), overlaid by tracked_config_ per entry id. Never
  // written; a new app version updates it wholesale.
  TrackedFoldersConfig base_config_;
  // The stamp both sides agreed on at the last successful settings sync; mirrored to settings.txt.
  long long backup_settings_synced_{};
  // Set when tracked-folders.json was present but could not be read or parsed. While set, the
  // config must never be written back, so a truncated or corrupt file is never overwritten; every
  // write path checks this flag before saving.
  bool tracked_config_load_failed_{};
  // Triangle pressed on the live-save row while its time was still resolving: open Save Details as
  // soon as the resolve lands instead of swallowing the press. Any other input cancels it.
  bool details_open_pending_{};
  // Details is a separate input/rendering mode. Keeping its state here lets Ui stay I/O-free.
  SlotDetailsState slot_details_;
  // The data-folder picker, another separate input/rendering mode (open gates its branch).
  DirectoryBrowserState directory_browser_;
  // Countdown (frames) until the browser starts sizing the focused row; -1 when idle or already
  // known. Debounces the walk so scrolling the list does not size every folder it passes.
  int pending_browser_size_frames_{-1};
  // State of the incremental size walk: a depth-first stack with one open directory per level, so
  // every SUBdirectory's total is known the moment the walk unwinds through it (deepest branches
  // first) and lands in the cache - sizing VitaDB once also sizes everything inside it.
  struct BrowserSizeWalkFrame {
    std::string path;
    DIR *dir{};
    std::uint64_t bytes{};
  };
  struct BrowserSizeWalk {
    bool active{};
    std::string target;  // the folder whose row is spinning
    std::vector<BrowserSizeWalkFrame> stack;
  };
  BrowserSizeWalk browser_size_walk_;
  // Folders the cursor rested on while another walk was running, measured in that order once it
  // finishes; the focused row always goes first.
  std::vector<std::string> browser_size_queue_;
  // Sizes measured this visit, keyed by full path, so re-tagging or re-entering a directory never
  // wipes what was already walked. Cleared when the picker opens.
  std::map<std::string, std::uint64_t> browser_size_cache_;
  bool enter_is_cross_{true};
  // Device-flow state: while a device code is active the main loop polls Google automatically at
  // the server-provided interval, so connecting only takes one Triangle press plus phone approval.
  bool google_auth_pending_{};
  int auth_poll_interval_seconds_{5};
  int auth_poll_delay_frames_{};
  int auth_poll_failures_{};
  long long device_code_expires_at_{};
  std::string status_message_;
  StatusKind status_kind_{StatusKind::Info};
  Ui ui_;
};

} // namespace vsm::vita
