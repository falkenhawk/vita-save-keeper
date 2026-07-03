#pragma once

#include "core/GoogleAuth.hpp"
#include "core/GoogleConfig.hpp"
#include "core/SaveCategory.hpp"
#include "core/SaveRecord.hpp"
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
};

class App {
public:
  int run();

private:
  void refresh_local_backups();
  void move_selected_save(int delta);
  void move_selected_backup(int delta);
  void move_selected_category(int delta);
  void rebuild_visible_saves();
  std::size_t category_count(SaveCategory category) const;
  const SaveRecord *selected_save_record() const;
  void cancel_restore_confirmation();
  void cancel_delete_confirmation();
  void handle_action_button();
  void create_new_backup();
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
  std::string find_or_create_drive_folder(const std::string &folder_name,
                                          const std::string &parent_id);
  bool sync_drive_index();
  void refresh_remote_backups_view();
  std::vector<std::string> remote_backup_names() const;
  std::size_t backup_count() const;
  bool selected_backup_is_remote() const;
  std::string selected_backup_name() const;
  void handle_upload_button();
  void set_status(StatusKind kind, std::string message);

  GoogleClientCredentials google_credentials_;
  GoogleTokenCache google_token_cache_;
  DeviceCodeResponse device_code_;
  std::vector<SaveRecord> saves_;
  // Indices into saves_ for the active category tab; selected_save_ indexes this list.
  std::vector<std::size_t> visible_saves_;
  SaveCategory category_{SaveCategory::VitaGame};
  // Which save was focused in each category tab, so L/R returns to where the user left off.
  std::array<std::size_t, kSaveCategoryCount> category_selection_{};
  std::vector<std::string> local_backups_;
  // remote_backups_ is the per-save view derived from drive_index_, which holds every backup in
  // Drive keyed by save folder name. The index is filled by one paginated sync instead of a
  // network round-trip per selected game.
  std::vector<RemoteBackup> remote_backups_;
  std::map<std::string, std::vector<RemoteBackup>> drive_index_;
  std::unordered_map<std::string, std::string> drive_folder_ids_;
  std::string drive_root_folder_id_;
  bool drive_synced_{};
  std::size_t selected_save_{};
  // Index into the backup menu: 0 is the "New Backup" entry, backups follow at 1..backup_count().
  std::size_t selected_backup_{};
  bool restore_confirmation_pending_{};
  bool delete_confirmation_pending_{};
  bool google_connected_{};
  // Set when sign-in completes so the first Drive listing runs one frame later, after the
  // "connected" state has been drawn, instead of freezing the sign-in screen.
  bool pending_remote_refresh_{};
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
