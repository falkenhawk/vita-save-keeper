#pragma once

#include "core/GoogleAuth.hpp"
#include "core/GoogleConfig.hpp"
#include "core/SaveRecord.hpp"
#include "vita/net/HttpClient.hpp"
#include "vita/ui/Ui.hpp"

#include <cstddef>
#include <functional>
#include <string>
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
  void cancel_restore_confirmation();
  void handle_restore_button();
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
  void refresh_remote_backups();
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
  std::vector<std::string> local_backups_;
  std::vector<RemoteBackup> remote_backups_;
  std::size_t selected_save_{};
  std::size_t selected_backup_{};
  bool restore_confirmation_pending_{};
  bool google_connected_{};
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
