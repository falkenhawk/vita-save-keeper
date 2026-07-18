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
#include "vita/net/HttpClient.hpp"
#include "vita/ui/Ui.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <map>
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
  void move_selected_backup(int delta);
  void move_selected_category(int delta);
  void cycle_sort_mode();
  void apply_sort_and_rebuild();
  std::map<std::string, std::string> newest_remote_by_folder() const;
  void load_settings();
  void save_settings();
  void rebuild_visible_saves();
  void schedule_selected_save_time_resolve();
  void resolve_selected_save_time();
  // Returns false if the user canceled (Square) mid-read, so the caller can keep the name order.
  bool resolve_all_save_times();
  bool resolve_save_time(SaveRecord *save);
  // Fills save times from save_time_cache_ for every save whose folder fingerprint still matches,
  // clearing save_time_requires_mount so those saves skip the mount entirely.
  void apply_save_time_cache();
  void flush_save_time_cache();
  // Adds the scan's mount-free time results to the cache and flushes it.
  void store_scanned_save_times();
  // Rebuilds the title cache from the scanned records against the given app-database stamp,
  // writing only when something actually changed.
  void rebuild_save_title_cache(long long app_db_mtime, long long app_db_size);
  // After a restore rewrote the live folder: drop the cache entry and re-derive the record's time
  // state so the grid never keeps showing the pre-restore save time.
  void invalidate_save_time(const SaveRecord &restored);
  std::size_t category_count(SaveCategory category) const;
  const SaveRecord *selected_save_record() const;
  void cancel_restore_confirmation();
  void cancel_delete_confirmation();
  void handle_action_button();
  void create_new_backup();
  LocalSnapshotResult create_local_snapshot(const SaveRecord &save, const std::string &suffix,
                                            bool report_progress, bool force_new = false);
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
  void download_and_inspect_selected_backup();
  void open_save_details();
  void request_save_details();
  void repair_remote_backup_metadata(const SaveRecord &save, const BackupRow &row,
                                     bool replace_unusable = false);
  bool remote_backup_exists(const std::string &save_id, const std::string &backup_name) const;
  void begin_sync_all();
  void run_sync_all();
  void cancel_sync_all_confirmation();
  void cancel_duplicate_backup_confirmation();
  bool poll_batch_cancel();
  void set_status(StatusKind kind, std::string message);
  void clear_status();

  GoogleClientCredentials google_credentials_;
  GoogleTokenCache google_token_cache_;
  DeviceCodeResponse device_code_;
  std::vector<SaveRecord> saves_;
  // Indices into saves_ for the active category tab; selected_save_ indexes this list.
  std::vector<std::size_t> visible_saves_;
  SaveCategory category_{SaveCategory::VitaGame};
  SaveSortMode sort_mode_{SaveSortMode::Name};
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
  // Set while run_sync_all is executing; the HTTP cancel check polls the pad only then, so a
  // held cancel button can abort an in-flight upload without affecting single-file transfers.
  bool batch_running_{};
  bool batch_cancel_requested_{};
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
  // Mount-resolved save times keyed by save id, trusted while each save folder's fingerprint is
  // unchanged. Loaded once at startup; dirty entries are flushed after each resolve batch.
  SaveTimeCache save_time_cache_;
  bool save_time_cache_dirty_{};
  // Title metadata keyed by save id; app-database entries follow the database stamp, sfo-derived
  // ones the folder fingerprint. Rebuilt (and written only on change) after every scan.
  SaveTitleCache save_title_cache_;
  // Countdown (frames) until the focused save's mount-only time is read; -1 when nothing pending.
  // Reset on every navigation so scrolling debounces the blocking mount. See the main loop tick.
  int pending_time_resolve_frames_{-1};
  // Triangle pressed on the live-save row while its time was still resolving: open Save Details as
  // soon as the resolve lands instead of swallowing the press. Any other input cancels it.
  bool details_open_pending_{};
  // Details is a separate input/rendering mode. Keeping its state here lets Ui stay I/O-free.
  SlotDetailsState slot_details_;
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
