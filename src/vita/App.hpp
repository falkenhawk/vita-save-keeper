#pragma once

#include "core/GoogleAuth.hpp"
#include "core/GoogleConfig.hpp"
#include "core/SaveRecord.hpp"
#include "vita/ui/Ui.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace vsm::vita {

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

  GoogleClientCredentials google_credentials_;
  DeviceCodeResponse device_code_;
  std::vector<SaveRecord> saves_;
  std::vector<std::string> local_backups_;
  std::size_t selected_save_{};
  std::size_t selected_backup_{};
  bool restore_confirmation_pending_{};
  bool google_connected_{};
  bool google_auth_pending_{};
  std::string status_message_;
  Ui ui_;
};

} // namespace vsm::vita
