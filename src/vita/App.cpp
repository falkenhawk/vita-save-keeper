#include "vita/App.hpp"

#include "core/BackupArchive.hpp"
#include "core/BackupStore.hpp"
#include "core/GoogleAuth.hpp"
#include "core/GoogleConfig.hpp"
#include "core/GoogleDrive.hpp"
#include "core/PathUtil.hpp"
#include "core/SaveScanner.hpp"
#include "core/Selection.hpp"
#include "vita/net/HttpClient.hpp"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace vsm::vita {
namespace {

constexpr int kFrameDelayUs = 16 * 1000;
constexpr const char *kDataRoot = "ux0:data/save-keeper";
constexpr const char *kBackupRoot = "ux0:data/save-keeper/backups";
constexpr const char *kGoogleClientPath = "ux0:data/save-keeper/google-client.json";
constexpr const char *kGoogleTokenPath = "ux0:data/save-keeper/google-token.json";
constexpr const char *kDriveFilesEndpoint = "https://www.googleapis.com/drive/v3/files";
constexpr const char *kDriveUploadEndpoint =
    "https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart&fields=id%2Cname";

std::vector<SaveRoot> default_save_roots() {
  return {
      {SavePlatform::Vita, "ux0:user/00/savedata"},
      {SavePlatform::Psp, "ux0:pspemu/PSP/SAVEDATA"},
  };
}

BackupTimestamp current_backup_timestamp() {
  const std::time_t now = std::time(nullptr);
  const std::tm *local = std::localtime(&now);
  if (!local) {
    return {1980, 1, 1, 0, 0};
  }

  return {
      local->tm_year + 1900,
      local->tm_mon + 1,
      local->tm_mday,
      local->tm_hour,
      local->tm_min,
  };
}

long long current_epoch_seconds() {
  return static_cast<long long>(std::time(nullptr));
}

bool ensure_directory(const char *path) {
  if (mkdir(path, 0777) == 0 || errno == EEXIST) {
    return true;
  }
  return false;
}

bool read_text_file(const char *path, std::string *contents) {
  FILE *file = std::fopen(path, "rb");
  if (!file) {
    return false;
  }

  contents->clear();
  char buffer[4096];
  while (true) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    if (read > 0) {
      contents->append(buffer, read);
    }
    if (read < sizeof(buffer)) {
      const bool ok = std::ferror(file) == 0;
      std::fclose(file);
      return ok;
    }
  }
}

bool write_text_file(const char *path, const std::string &contents) {
  ensure_directory(kDataRoot);
  FILE *file = std::fopen(path, "wb");
  if (!file) {
    return false;
  }
  const bool ok = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
  return std::fclose(file) == 0 && ok;
}

std::string token_error_text(const TokenResponse &response) {
  if (!response.error_description.empty()) {
    return response.error + ": " + response.error_description;
  }
  return response.error.empty() ? "invalid token response" : response.error;
}

} // namespace

void App::refresh_local_backups() {
  if (saves_.empty()) {
    local_backups_.clear();
    selected_backup_ = 0;
    return;
  }

  const SaveRecord &save = saves_[selected_save_ % saves_.size()];
  local_backups_ = scan_local_backup_names(kBackupRoot, save.id);
  if (selected_backup_ >= local_backups_.size()) {
    selected_backup_ = 0;
  }
}

void App::move_selected_save(int delta) {
  const std::size_t previous = selected_save_;
  selected_save_ = move_selection(selected_save_, saves_.size(), delta);
  if (selected_save_ != previous) {
    cancel_restore_confirmation();
    refresh_local_backups();
  }
}

void App::move_selected_backup(int delta) {
  const std::size_t previous = selected_backup_;
  selected_backup_ = move_selection(selected_backup_, local_backups_.size(), delta);
  if (selected_backup_ != previous) {
    cancel_restore_confirmation();
  }
}

void App::cancel_restore_confirmation() {
  if (restore_confirmation_pending_) {
    restore_confirmation_pending_ = false;
    status_message_ = "Restore cancelled.";
  }
}

void App::handle_restore_button() {
  if (saves_.empty()) {
    status_message_ = "No save selected.";
    return;
  }
  if (local_backups_.empty()) {
    status_message_ = "No local backup selected.";
    return;
  }

  const std::string &backup_name = local_backups_[selected_backup_ % local_backups_.size()];
  if (!restore_confirmation_pending_) {
    restore_confirmation_pending_ = true;
    status_message_ = "Press Square again to restore " + backup_name + ".";
    return;
  }

  const SaveRecord &save = saves_[selected_save_ % saves_.size()];
  const RestoreResult result = restore_backup_archive({
      local_backup_archive_path(kBackupRoot, save.id, backup_name),
      save.path,
  });
  restore_confirmation_pending_ = false;
  status_message_ = result.ok ? "Restored " + backup_name : "Restore failed: " + result.error;
}

void App::load_google_token_cache() {
  std::string json;
  if (!read_text_file(kGoogleTokenPath, &json)) {
    google_connected_ = false;
    return;
  }

  google_token_cache_ = parse_google_token_cache(json);
  google_connected_ = google_token_cache_.ok;
}

bool App::load_google_credentials() {
  if (google_credentials_.ok) {
    return true;
  }

  std::string json;
  if (!read_text_file(kGoogleClientPath, &json)) {
    status_message_ = "Missing ux0:data/save-keeper/google-client.json.";
    return false;
  }

  google_credentials_ = parse_google_client_credentials(json);
  if (!google_credentials_.ok) {
    status_message_ = "Google client JSON needs client_id and client_secret.";
  }
  return google_credentials_.ok;
}

void App::handle_google_button() {
  if (!load_google_credentials()) {
    return;
  }

  HttpClient http;
  if (!google_auth_pending_) {
    const HttpResponse response =
        http.post_form(kGoogleDeviceCodeEndpoint,
                       build_device_code_request_body(google_credentials_.client_id));
    if (!response.ok) {
      status_message_ = response.error.empty() ? "Google device request failed."
                                               : "Google request failed: " + response.error;
      return;
    }

    device_code_ = parse_device_code_response(response.body);
    if (!device_code_.ok) {
      status_message_ = "Google device response invalid.";
      return;
    }

    google_auth_pending_ = true;
    status_message_ = "Open " + device_code_.verification_url + " code " + device_code_.user_code;
    return;
  }

  const HttpResponse response = http.post_form(
      kGoogleTokenEndpoint,
      build_device_token_request_body(google_credentials_.client_id, google_credentials_.client_secret,
                                      device_code_.device_code));
  const TokenResponse token = parse_token_response(response.body);
  if (token.ok) {
    GoogleTokenCache cache;
    cache.ok = true;
    cache.access_token = token.access_token;
    cache.refresh_token = token.refresh_token.empty() ? google_token_cache_.refresh_token
                                                      : token.refresh_token;
    cache.token_type = token.token_type;
    cache.expires_at_epoch_seconds = current_epoch_seconds() + token.expires_in;
    google_token_cache_ = cache;
    if (!save_google_token_cache()) {
      status_message_ = "Google connected, but token save failed.";
      return;
    }

    google_connected_ = true;
    google_auth_pending_ = false;
    device_code_ = {};
    status_message_ = "Google Drive connected.";
    return;
  }

  if (token.error == "authorization_pending") {
    status_message_ = "Waiting for browser approval.";
  } else if (token.error == "slow_down") {
    status_message_ = "Google asked to slow down before polling.";
  } else {
    google_auth_pending_ = false;
    status_message_ = "Google auth failed: " + token_error_text(token);
  }
}

bool App::save_google_token_cache() {
  return write_text_file(kGoogleTokenPath, serialize_google_token_cache(google_token_cache_));
}

bool App::refresh_google_access_token() {
  if (!load_google_credentials() || google_token_cache_.refresh_token.empty()) {
    return false;
  }

  HttpClient http;
  const HttpResponse response = http.post_form(
      kGoogleTokenEndpoint,
      build_refresh_token_request_body(google_credentials_.client_id, google_credentials_.client_secret,
                                       google_token_cache_.refresh_token));
  const TokenResponse token = parse_token_response(response.body);
  if (!token.ok) {
    status_message_ = "Google refresh failed: " + token_error_text(token);
    google_connected_ = false;
    return false;
  }

  google_token_cache_.access_token = token.access_token;
  google_token_cache_.ok = true;
  google_token_cache_.token_type = token.token_type;
  google_token_cache_.expires_at_epoch_seconds = current_epoch_seconds() + token.expires_in;
  google_connected_ = save_google_token_cache();
  return google_connected_;
}

bool App::ensure_google_access_token() {
  if (!google_token_cache_.ok) {
    status_message_ = "Connect Google Drive first.";
    return false;
  }
  if (!google_token_cache_.access_token.empty() &&
      google_token_cache_.expires_at_epoch_seconds > current_epoch_seconds() + 60) {
    return true;
  }
  return refresh_google_access_token();
}

std::string App::find_or_create_drive_folder(const std::string &folder_name,
                                             const std::string &parent_id) {
  HttpClient http;
  const std::string list_url = std::string(kDriveFilesEndpoint) + "?" +
                               build_drive_find_folder_query(folder_name, parent_id);
  const HttpResponse list_response = http.get_json(list_url, google_token_cache_.access_token);
  if (list_response.ok) {
    const DriveFileList files = parse_drive_file_list(list_response.body);
    if (files.ok && !files.files.empty()) {
      return files.files[0].id;
    }
  }

  const std::string create_url = std::string(kDriveFilesEndpoint) + "?fields=id%2Cname";
  const HttpResponse create_response = http.post_json(
      create_url, build_drive_folder_metadata_json(folder_name, parent_id),
      google_token_cache_.access_token);
  if (!create_response.ok) {
    status_message_ = "Drive folder create failed.";
    return {};
  }

  const DriveFileList created = parse_drive_file_list(create_response.body);
  if (!created.ok || created.files.empty()) {
    status_message_ = "Drive folder response invalid.";
    return {};
  }
  return created.files[0].id;
}

void App::handle_upload_button() {
  if (saves_.empty()) {
    status_message_ = "No save selected.";
    return;
  }
  if (local_backups_.empty()) {
    status_message_ = "No local backup selected.";
    return;
  }
  if (!ensure_google_access_token()) {
    return;
  }

  const SaveRecord &save = saves_[selected_save_ % saves_.size()];
  const std::string &backup_name = local_backups_[selected_backup_ % local_backups_.size()];
  const std::string archive_path = local_backup_archive_path(kBackupRoot, save.id, backup_name);
  const std::string root_id = find_or_create_drive_folder(kGoogleDriveRootFolderName, "root");
  if (root_id.empty()) {
    return;
  }

  std::string save_folder_name = normalize_path_component(save.id);
  if (save_folder_name.empty()) {
    save_folder_name = "unknown-save";
  }
  const std::string save_folder_id = find_or_create_drive_folder(save_folder_name, root_id);
  if (save_folder_id.empty()) {
    return;
  }

  HttpClient http;
  const HttpResponse upload_response = http.post_multipart_file(
      kDriveUploadEndpoint, build_drive_upload_metadata_json(backup_name, save_folder_id),
      archive_path, google_token_cache_.access_token);
  status_message_ = upload_response.ok ? "Uploaded [GD] " + backup_name
                                       : "Drive upload failed.";
}

int App::run() {
  if (!ui_.initialize()) {
    return -1;
  }

  // Scan once at startup for the foundation build. Later actions that create, restore, or delete a
  // save will refresh this list explicitly so the UI does not rescan storage every frame.
  saves_ = scan_save_roots(default_save_roots());
  refresh_local_backups();
  load_google_token_cache();

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  bool running = true;
  unsigned int previous_buttons = 0;
  while (running) {
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const unsigned int pressed = pad.buttons & ~previous_buttons;

    // START exits immediately in this foundation build. Later screens will route input through a
    // state stack so destructive actions can require explicit confirmation or hold-to-confirm.
    if ((pressed & SCE_CTRL_START) != 0) {
      running = false;
    }
    if ((pressed & SCE_CTRL_LEFT) != 0) {
      move_selected_save(-1);
    }
    if ((pressed & SCE_CTRL_RIGHT) != 0) {
      move_selected_save(1);
    }
    if ((pressed & SCE_CTRL_UP) != 0) {
      move_selected_save(-4);
    }
    if ((pressed & SCE_CTRL_DOWN) != 0) {
      move_selected_save(4);
    }
    if ((pressed & SCE_CTRL_LTRIGGER) != 0) {
      move_selected_backup(-1);
    }
    if ((pressed & SCE_CTRL_RTRIGGER) != 0) {
      move_selected_backup(1);
    }
    if ((pressed & SCE_CTRL_CIRCLE) != 0) {
      restore_confirmation_pending_ = false;
      if (saves_.empty()) {
        status_message_ = "No save selected.";
      } else {
        const SaveRecord &save = saves_[selected_save_ % saves_.size()];
        const BackupResult result = create_backup_archive({
            save.path,
            kBackupRoot,
            save.id,
            current_backup_timestamp(),
        });
        status_message_ = result.ok ? "Created " + result.archive_path
                                    : "Backup failed: " + result.error;
        if (result.ok) {
          refresh_local_backups();
        }
      }
    }
    if ((pressed & SCE_CTRL_SQUARE) != 0) {
      handle_restore_button();
    }
    if ((pressed & SCE_CTRL_CROSS) != 0) {
      cancel_restore_confirmation();
    }
    if ((pressed & SCE_CTRL_TRIANGLE) != 0) {
      handle_google_button();
    }
    if ((pressed & SCE_CTRL_SELECT) != 0) {
      handle_upload_button();
    }

    ui_.draw(saves_, selected_save_, local_backups_, selected_backup_,
             restore_confirmation_pending_, google_connected_, google_auth_pending_,
             device_code_.verification_url, device_code_.user_code, status_message_);
    previous_buttons = pad.buttons;

    // This keeps the placeholder loop from busy-spinning. Vita2d swaps on vblank when configured,
    // but the small delay also keeps CPU use reasonable if vblank wait is disabled by a future build.
    sceKernelDelayThread(kFrameDelayUs);
  }

  ui_.shutdown();
  sceKernelExitProcess(0);
  return 0;
}

} // namespace vsm::vita
