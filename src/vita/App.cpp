#include "vita/App.hpp"

#include "core/AppSettings.hpp"
#include "core/BackupArchive.hpp"
#include "core/BackupName.hpp"
#include "core/BackupStore.hpp"
#include "core/GoogleAuth.hpp"
#include "core/GoogleConfig.hpp"
#include "core/GoogleDrive.hpp"
#include "core/PathUtil.hpp"
#include "core/SaveScanner.hpp"
#include "core/Selection.hpp"
#include "vita/SaveAppDbMetadata.hpp"
#include "vita/net/HttpClient.hpp"

#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/system_param.h>
#include <algorithm>
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
// The loop runs at roughly 60 frames per second (16 ms delay plus vblank), so poll timers are
// counted in frames instead of wall-clock threads to keep the app single-threaded.
constexpr int kFramesPerSecond = 60;
// One dropped request should not abort a sign-in, but a dead connection should stop the flow
// instead of polling forever.
constexpr int kAuthMaxPollFailures = 5;
constexpr const char *kDataRoot = "ux0:data/save-keeper";
constexpr const char *kBackupRoot = "ux0:data/save-keeper/backups";
constexpr const char *kGoogleClientPath = "ux0:data/save-keeper/google-client.json";
constexpr const char *kGoogleTokenPath = "ux0:data/save-keeper/google-token.json";
constexpr const char *kSettingsPath = "ux0:data/save-keeper/settings.txt";
constexpr const char *kDriveFilesEndpoint = "https://www.googleapis.com/drive/v3/files";
constexpr const char *kDriveUploadEndpoint =
    "https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart&fields=id%2Cname";
constexpr int kAnalogCenter = 128;
constexpr int kAnalogDeadZone = 48;
constexpr int kRepeatInitialDelayFrames = 18;
constexpr int kRepeatIntervalFrames = 5;
constexpr unsigned int kRepeatableButtons = SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_UP |
                                            SCE_CTRL_DOWN | SCE_CTRL_LTRIGGER |
                                            SCE_CTRL_RTRIGGER;
// Holding Select runs the tab-wide backup & upload batch; a tap keeps its single-upload meaning.
// The hint gauge starts a few frames in so a quick tap never flashes it.
constexpr int kSelectHoldTriggerFrames = 60;
constexpr int kSelectHoldHintFrames = 12;

std::vector<SaveRoot> default_save_roots() {
  return {
      {SavePlatform::Vita, "ux0:user/00/savedata"},
      {SavePlatform::GameCard, "grw0:savedata"},
      {SavePlatform::Psp, "ux0:pspemu/PSP/SAVEDATA"},
  };
}

BackupTimestamp current_backup_timestamp() {
  const std::time_t now = std::time(nullptr);
  const std::tm *local = std::localtime(&now);
  if (!local) {
    return {1980, 1, 1, 0, 0, 0};
  }

  return {
      local->tm_year + 1900,
      local->tm_mon + 1,
      local->tm_mday,
      local->tm_hour,
      local->tm_min,
      local->tm_sec,
  };
}

long long current_epoch_seconds() {
  return static_cast<long long>(std::time(nullptr));
}

unsigned int buttons_with_left_analog(const SceCtrlData &pad) {
  unsigned int buttons = pad.buttons;

  if (pad.lx < kAnalogCenter - kAnalogDeadZone) {
    buttons |= SCE_CTRL_LEFT;
  } else if (pad.lx > kAnalogCenter + kAnalogDeadZone) {
    buttons |= SCE_CTRL_RIGHT;
  }

  if (pad.ly < kAnalogCenter - kAnalogDeadZone) {
    buttons |= SCE_CTRL_UP;
  } else if (pad.ly > kAnalogCenter + kAnalogDeadZone) {
    buttons |= SCE_CTRL_DOWN;
  }

  return buttons;
}

unsigned int apply_hold_repeat(unsigned int buttons, unsigned int previous_buttons,
                               unsigned int *repeat_held_buttons, int *repeat_frames) {
  unsigned int pressed = buttons & ~previous_buttons;
  const unsigned int held_repeatable = buttons & kRepeatableButtons;

  // VitaShell-style menus repeat only navigation controls. Action buttons stay edge-triggered so
  // holding Circle, Square, Triangle, or Select cannot create duplicate backups or auth requests.
  if (held_repeatable == 0 || held_repeatable != *repeat_held_buttons) {
    *repeat_held_buttons = held_repeatable;
    *repeat_frames = 0;
    return pressed;
  }

  ++(*repeat_frames);
  if (*repeat_frames >= kRepeatInitialDelayFrames &&
      ((*repeat_frames - kRepeatInitialDelayFrames) % kRepeatIntervalFrames) == 0) {
    pressed |= held_repeatable;
  }

  return pressed;
}

bool ensure_directory(const char *path) {
  if (mkdir(path, 0777) == 0 || errno == EEXIST) {
    return true;
  }
  return false;
}

bool ensure_directory_path(const std::string &path) {
  std::string current;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::size_t end = slash == std::string::npos ? path.size() : slash;
    const std::string part = path.substr(start, end - start);
    if (!part.empty()) {
      if (!current.empty()) {
        current += "/";
      }
      current += part;
      if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST) {
        return false;
      }
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

bool ensure_parent_directory(const std::string &path) {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return true;
  }
  return ensure_directory_path(path.substr(0, slash));
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

// Name of the local archive whose contents equal the given folder signature, or empty. Content is
// compared against every archive, not just the newest: matching an older one still means the
// bytes are preserved, and a new zip of them would only duplicate it under another timestamp.
std::string matching_backup_name(const std::vector<ArchiveEntryInfo> &entries,
                                 const std::string &save_id,
                                 const std::vector<std::string> &backup_names) {
  for (const std::string &existing : backup_names) {
    if (entries_match_backup_archive(entries,
                                     local_backup_archive_path(kBackupRoot, save_id, existing))) {
      return existing;
    }
  }
  return {};
}

std::string archive_file_name(const std::string &archive_path) {
  const std::size_t slash = archive_path.find_last_of('/');
  return slash == std::string::npos ? archive_path : archive_path.substr(slash + 1);
}

std::string drive_folder_name_for(const std::string &save_id) {
  std::string folder_name = normalize_path_component(save_id);
  if (folder_name.empty()) {
    folder_name = "unknown-save";
  }
  return folder_name;
}

} // namespace

void App::set_status(StatusKind kind, std::string message) {
  status_kind_ = kind;
  status_message_ = std::move(message);
}

void App::clear_status() {
  status_kind_ = StatusKind::Info;
  status_message_.clear();
}

void App::load_settings() {
  std::string text;
  if (read_text_file(kSettingsPath, &text)) {
    sort_mode_ = parse_app_settings(text).sort_mode;
  }
}

void App::save_settings() {
  AppSettings settings;
  settings.sort_mode = sort_mode_;
  write_text_file(kSettingsPath, serialize_app_settings(settings));
}

std::map<std::string, std::string> App::newest_remote_by_folder() const {
  // Keyed by the bare save key (what apply_save_sort derives from each save), resolving folders
  // that carry a game title after the key.
  std::map<std::string, std::string> newest;
  for (const SaveRecord &save : saves_) {
    const std::string folder_name = resolved_drive_folder_name(save.id);
    if (folder_name.empty()) {
      continue;
    }
    const auto entry = drive_index_.find(folder_name);
    if (entry != drive_index_.end() && !entry->second.empty()) {
      // Per-save lists are sorted newest first, so the first name is the latest sync point.
      newest[drive_folder_name_for(save.id)] = entry->second[0].name;
    }
  }
  return newest;
}

void App::apply_sort_and_rebuild() {
  std::string focused_id;
  if (const SaveRecord *current = selected_save_record()) {
    focused_id = current->id;
  }

  apply_save_sort(&saves_, sort_mode_, newest_remote_by_folder());
  // Remembered per-tab positions point into the old order; the current save is re-located by id
  // instead so the focus survives the re-sort (the grid window follows it on the next frame).
  category_selection_.fill(0);
  rebuild_visible_saves();
  selected_save_ = 0;
  if (!focused_id.empty()) {
    for (std::size_t i = 0; i < visible_saves_.size(); ++i) {
      if (saves_[visible_saves_[i]].id == focused_id) {
        selected_save_ = i;
        break;
      }
    }
  }
  category_selection_[static_cast<std::size_t>(category_)] = selected_save_;
  refresh_local_backups();
  refresh_remote_backups_view();
}

void App::cycle_sort_mode() {
  // Re-sorting reorders saves_, which the cached batch plan indexes into.
  cancel_sync_all_confirmation();
  cancel_duplicate_backup_confirmation();
  sort_mode_ =
      static_cast<SaveSortMode>((static_cast<int>(sort_mode_) + 1) % kSaveSortModeCount);
  apply_sort_and_rebuild();
  save_settings();
  switch (sort_mode_) {
  case SaveSortMode::LastSaved:
    set_status(StatusKind::Info, "Sorted by last saved.");
    break;
  case SaveSortMode::LastSynced:
    set_status(StatusKind::Info, "Sorted by last synced.");
    break;
  case SaveSortMode::Name:
  default:
    set_status(StatusKind::Info, "Sorted by name.");
    break;
  }
}

std::size_t App::category_count(SaveCategory category) const {
  std::size_t count = 0;
  for (const SaveRecord &save : saves_) {
    if (classify_save(save) == category) {
      ++count;
    }
  }
  return count;
}

void App::rebuild_visible_saves() {
  visible_saves_.clear();
  for (std::size_t i = 0; i < saves_.size(); ++i) {
    if (classify_save(saves_[i]) == category_) {
      visible_saves_.push_back(i);
    }
  }
  if (selected_save_ >= visible_saves_.size()) {
    selected_save_ = 0;
  }
}

const SaveRecord *App::selected_save_record() const {
  if (visible_saves_.empty()) {
    return nullptr;
  }
  return &saves_[visible_saves_[selected_save_ % visible_saves_.size()]];
}

void App::refresh_local_backups() {
  const SaveRecord *save = selected_save_record();
  if (!save) {
    local_backups_.clear();
    selected_backup_ = 0;
    return;
  }

  local_backups_ = scan_local_backup_names(kBackupRoot, save->id);
  if (selected_backup_ > backup_count()) {
    selected_backup_ = 0;
  }
}

void App::move_selected_save(int delta) {
  const std::size_t previous = selected_save_;
  selected_save_ = move_selection(selected_save_, visible_saves_.size(), delta);
  if (selected_save_ != previous) {
    cancel_restore_confirmation();
    cancel_delete_confirmation();
    cancel_sync_all_confirmation();
    cancel_duplicate_backup_confirmation();
    // A different save means a different backup list; focus its "New Backup" entry. Any status
    // message described the previous save, so it goes too.
    selected_backup_ = 0;
    refresh_local_backups();
    refresh_remote_backups_view();
    clear_status();
  }
}

void App::move_selected_category(int delta) {
  // Cycle through the category tabs, skipping empty ones so L/R always lands on content.
  int index = static_cast<int>(category_);
  for (int step = 0; step < kSaveCategoryCount; ++step) {
    index = (index + delta + kSaveCategoryCount) % kSaveCategoryCount;
    const SaveCategory candidate = static_cast<SaveCategory>(index);
    if (category_count(candidate) == 0) {
      continue;
    }
    if (candidate == category_) {
      return;
    }
    category_selection_[static_cast<std::size_t>(category_)] = selected_save_;
    category_ = candidate;
    cancel_restore_confirmation();
    cancel_delete_confirmation();
    cancel_sync_all_confirmation();
    cancel_duplicate_backup_confirmation();
    selected_save_ = category_selection_[static_cast<std::size_t>(category_)];
    selected_backup_ = 0;
    rebuild_visible_saves();
    refresh_local_backups();
    refresh_remote_backups_view();
    clear_status();
    return;
  }
}

void App::move_selected_backup(int delta) {
  const std::size_t previous = selected_backup_;
  // Menu size is the backups plus the "New Backup" entry at index 0.
  selected_backup_ = move_selection(selected_backup_, backup_count() + 1, delta);
  if (selected_backup_ != previous) {
    cancel_restore_confirmation();
    cancel_delete_confirmation();
    cancel_sync_all_confirmation();
    // The "press again to force" state refers to the New Backup entry; leaving it must not arm a
    // silent force for later.
    cancel_duplicate_backup_confirmation();
  }
}

void App::cancel_restore_confirmation() {
  if (restore_confirmation_pending_) {
    restore_confirmation_pending_ = false;
    set_status(StatusKind::Info, "Restore cancelled.");
  }
}

void App::cancel_delete_confirmation() {
  if (delete_confirmation_pending_) {
    delete_confirmation_pending_ = false;
    set_status(StatusKind::Info, "Delete cancelled.");
  }
}

void App::create_new_backup() {
  restore_confirmation_pending_ = false;
  delete_confirmation_pending_ = false;
  const SaveRecord *selected = selected_save_record();
  if (!selected) {
    set_status(StatusKind::Info, "No save selected.");
    return;
  }

  const SaveRecord &save = *selected;
  if (!duplicate_backup_confirmation_pending_) {
    // Content identical to an existing archive would only stack a same-bytes snapshot under a new
    // timestamp; warn first, and let a second press force it anyway (the batch never forces).
    ui_.draw_busy("Checking current save", 0, -1);
    bool signature_ok = false;
    const std::vector<ArchiveEntryInfo> entries = compute_folder_entries(save.path, &signature_ok);
    if (signature_ok && !entries.empty()) {
      const std::string match = matching_backup_name(entries, save.id, local_backups_);
      if (!match.empty()) {
        duplicate_backup_confirmation_pending_ = true;
        // The footer carries the "confirm / cancel" instruction; the status line only needs the
        // fact, so the archive name fits without truncation.
        set_status(StatusKind::Info, "Identical to " + display_backup_name(match));
        return;
      }
    }
  }
  duplicate_backup_confirmation_pending_ = false;

  // One busy frame before the blocking ZIP work, so the screen does not look frozen.
  ui_.draw_busy("Creating backup", 0, -1);
  const BackupResult result = create_backup_archive({
      save.path,
      kBackupRoot,
      save.id,
      current_backup_timestamp(),
  });
  if (result.ok) {
    refresh_local_backups();
    // Focus the fresh snapshot so an immediate Select-to-upload needs no scrolling. Local
    // backups sort newest first, but matching by name keeps this correct regardless.
    const std::string file_name = archive_file_name(result.archive_path);
    for (std::size_t i = 0; i < local_backups_.size(); ++i) {
      if (local_backups_[i] == file_name) {
        selected_backup_ = 1 + remote_backups_.size() + i;
        break;
      }
    }
    set_status(StatusKind::Success, "Created " + display_backup_name(file_name));
  } else {
    set_status(StatusKind::Error, "Backup failed: " + result.error);
  }
}

void App::handle_delete_button() {
  cancel_sync_all_confirmation();
  cancel_duplicate_backup_confirmation();
  const SaveRecord *selected = selected_save_record();
  if (!selected) {
    set_status(StatusKind::Info, "No save selected.");
    return;
  }
  if (selected_backup_ == 0 || backup_count() == 0) {
    set_status(StatusKind::Info, "Select a backup to delete.");
    return;
  }

  const std::string backup_name = selected_backup_name();
  if (!delete_confirmation_pending_) {
    restore_confirmation_pending_ = false;
    delete_confirmation_pending_ = true;
    set_status(StatusKind::Info, "Delete " + display_backup_name(backup_name) + "?");
    return;
  }
  delete_confirmation_pending_ = false;

  if (selected_backup_is_remote()) {
    if (!ensure_google_access_token()) {
      return;
    }
    const std::size_t remote_index = (selected_backup_ - 1) % remote_backups_.size();
    const RemoteBackup remote = remote_backups_[remote_index];
    BusyLabelScope busy("Deleting Drive backup");
    const std::string url =
        std::string(kDriveFilesEndpoint) + "/" + form_url_encode(remote.file_id);
    const HttpResponse response = drive_request([&](const std::string &token) {
      return HttpClient().delete_request(url, token);
    });
    if (!response.ok) {
      set_status(StatusKind::Error, "Drive delete failed.");
      return;
    }
    const std::string folder_name = resolved_drive_folder_name(selected->id);
    const auto indexed = drive_index_.find(folder_name);
    if (indexed != drive_index_.end()) {
      std::vector<RemoteBackup> &list = indexed->second;
      for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].file_id == remote.file_id) {
          list.erase(list.begin() + static_cast<long>(i));
          break;
        }
      }
      if (list.empty()) {
        drive_index_.erase(indexed);
        remove_drive_folder_if_empty(folder_name);
      }
    }
    refresh_remote_backups_view();
    if (selected_backup_ > backup_count()) {
      selected_backup_ = backup_count();
    }
    set_status(StatusKind::Success, "Deleted [GD] " + display_backup_name(backup_name) + ".");
    return;
  }

  const SaveRecord &save = *selected;
  const std::string archive_path = local_backup_archive_path(kBackupRoot, save.id, backup_name);
  if (std::remove(archive_path.c_str()) != 0) {
    set_status(StatusKind::Error,
               "Could not delete " + display_backup_name(backup_name) + ".");
    return;
  }
  refresh_local_backups();
  if (selected_backup_ > backup_count()) {
    selected_backup_ = backup_count();
  }
  set_status(StatusKind::Success, "Deleted " + display_backup_name(backup_name) + ".");
}

void App::handle_action_button() {
  if (sync_all_confirmation_pending_) {
    run_sync_all();
    return;
  }
  const SaveRecord *selected = selected_save_record();
  if (!selected) {
    set_status(StatusKind::Info, "No save selected.");
    return;
  }
  // One context-sensitive action button: the "New Backup" entry creates a snapshot, a backup
  // entry restores it (with a second press to confirm).
  if (selected_backup_ == 0) {
    create_new_backup();
    return;
  }
  handle_restore();
}

void App::handle_restore() {
  const SaveRecord *selected = selected_save_record();
  if (!selected || selected_backup_ == 0 || backup_count() == 0) {
    return;
  }
  const std::string backup_name = selected_backup_name();
  if (!restore_confirmation_pending_) {
    delete_confirmation_pending_ = false;
    restore_confirmation_pending_ = true;
    set_status(StatusKind::Info, "Restore " + display_backup_name(backup_name) + "?");
    return;
  }

  const SaveRecord &save = *selected;
  std::string archive_path = local_backup_archive_path(kBackupRoot, save.id, backup_name);
  const bool remote_restore = selected_backup_is_remote();

  // Safety net: snapshot the current save before overwriting it, unless some local backup
  // already holds exactly this content (compared by per-file path, size, and CRC32, because
  // file timestamps change on every restore and cannot be trusted).
  ui_.draw_busy("Checking current save", 0, -1);
  bool signature_ok = false;
  const std::vector<ArchiveEntryInfo> current_entries =
      compute_folder_entries(save.path, &signature_ok);
  if (signature_ok && !current_entries.empty()) {
    const bool already_backed_up =
        !matching_backup_name(current_entries, save.id, local_backups_).empty();
    if (!already_backed_up) {
      ui_.draw_busy("Backing up current save", 0, -1);
      BackupRequest auto_request;
      auto_request.source_path = save.path;
      auto_request.backup_root = kBackupRoot;
      auto_request.save_id = save.id;
      auto_request.timestamp = current_backup_timestamp();
      auto_request.name_suffix = " auto";
      const BackupResult auto_result = create_backup_archive(auto_request);
      if (!auto_result.ok) {
        // Losing the current save is the one outcome this feature exists to prevent; a restore
        // does not proceed over a failed safety snapshot.
        restore_confirmation_pending_ = false;
        set_status(StatusKind::Error, "Could not back up current save: " + auto_result.error);
        return;
      }
      refresh_local_backups();
      if (!remote_restore) {
        // The new snapshot shifted the local indices; re-locate the entry being restored.
        for (std::size_t i = 0; i < local_backups_.size(); ++i) {
          if (local_backups_[i] == backup_name) {
            selected_backup_ = 1 + remote_backups_.size() + i;
            break;
          }
        }
      }
    }
  }

  if (remote_restore) {
    if (!ensure_google_access_token()) {
      restore_confirmation_pending_ = false;
      return;
    }
    const RemoteBackup &remote = remote_backups_[(selected_backup_ - 1) % remote_backups_.size()];
    if (!ensure_parent_directory(archive_path)) {
      set_status(StatusKind::Error, "Could not create local backup folder.");
      restore_confirmation_pending_ = false;
      return;
    }
    BusyLabelScope busy("Downloading backup");
    const std::string download_url = std::string(kDriveFilesEndpoint) + "/" +
                                     form_url_encode(remote.file_id) + "?alt=media";
    const HttpResponse download = drive_request([&](const std::string &token) {
      return HttpClient().download_file(download_url, archive_path, token);
    });
    if (!download.ok) {
      restore_confirmation_pending_ = false;
      set_status(StatusKind::Error, "Drive download failed.");
      return;
    }
    refresh_local_backups();
  }

  ui_.draw_busy("Restoring save", 0, -1);
  const RestoreResult result = restore_backup_archive({
      archive_path,
      save.path,
  });
  restore_confirmation_pending_ = false;
  if (result.ok) {
    set_status(StatusKind::Success,
               remote_restore ? "Downloaded and restored [GD] " + display_backup_name(backup_name)
                              : "Restored " + display_backup_name(backup_name));
  } else {
    set_status(StatusKind::Error, "Restore failed: " + result.error);
  }
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

  google_credentials_ = embedded_google_client_credentials();
  if (google_credentials_.ok) {
    return true;
  }

  std::string json;
  if (!read_text_file(kGoogleClientPath, &json)) {
    set_status(StatusKind::Error, "No Google client set up - see docs/google-drive-setup.md");
    return false;
  }

  google_credentials_ = parse_google_client_credentials(json);
  if (!google_credentials_.ok) {
    set_status(StatusKind::Error, "Google client JSON needs client_id and client_secret.");
  }
  return google_credentials_.ok;
}

void App::handle_google_button() {
  // A refresh rewrites the Drive index the cached batch plan was computed from.
  cancel_sync_all_confirmation();
  cancel_duplicate_backup_confirmation();
  if (google_auth_pending_) {
    // Skip the remaining wait and check with Google right away.
    auth_poll_delay_frames_ = 0;
    return;
  }
  if (google_connected_) {
    if (sync_drive_index()) {
      refresh_remote_backups_view();
      if (sort_mode_ == SaveSortMode::LastSynced) {
        apply_sort_and_rebuild();
      }
      // The overlay already showed the sync happening; whatever status was left over predates
      // the fresh listing.
      clear_status();
    }
    return;
  }
  begin_google_auth();
}

void App::begin_google_auth() {
  if (!load_google_credentials()) {
    return;
  }

  BusyLabelScope busy("Contacting Google");
  HttpClient http;
  const HttpResponse response =
      http.post_form(kGoogleDeviceCodeEndpoint,
                     build_device_code_request_body(google_credentials_.client_id));
  if (!response.ok) {
    set_status(StatusKind::Error,
               response.error.empty()
                   ? "Google device request failed (HTTP " + std::to_string(response.status) + ")."
                   : "Google request failed: " + response.error);
    return;
  }

  device_code_ = parse_device_code_response(response.body);
  if (!device_code_.ok) {
    set_status(StatusKind::Error, "Google device response invalid.");
    return;
  }

  google_auth_pending_ = true;
  auth_poll_interval_seconds_ = device_code_.interval > 0 ? device_code_.interval : 5;
  auth_poll_delay_frames_ = auth_poll_interval_seconds_ * kFramesPerSecond;
  auth_poll_failures_ = 0;
  // Google device codes are valid for around 30 minutes; keep a fallback in case the field is
  // missing so the flow still times out instead of polling forever.
  device_code_expires_at_ =
      current_epoch_seconds() + (device_code_.expires_in > 0 ? device_code_.expires_in : 1800);
  set_status(StatusKind::Info, "Scan the QR code with your phone and approve access.");
}

void App::cancel_google_auth() {
  if (!google_auth_pending_) {
    return;
  }
  google_auth_pending_ = false;
  device_code_ = {};
  set_status(StatusKind::Info, "Google sign-in cancelled.");
}

void App::update_google_auth() {
  if (!google_auth_pending_) {
    return;
  }

  if (current_epoch_seconds() >= device_code_expires_at_) {
    google_auth_pending_ = false;
    device_code_ = {};
    set_status(StatusKind::Error, "Sign-in code expired. Press Triangle for a new code.");
    return;
  }

  if (auth_poll_delay_frames_ > 0) {
    --auth_poll_delay_frames_;
    return;
  }
  poll_google_token();
}

void App::poll_google_token() {
  auth_poll_delay_frames_ = auth_poll_interval_seconds_ * kFramesPerSecond;

  HttpClient http;
  const HttpResponse response = http.post_form(
      kGoogleTokenEndpoint,
      build_device_token_request_body(google_credentials_.client_id,
                                      google_credentials_.client_secret,
                                      device_code_.device_code));
  if (!response.ok && response.body.empty()) {
    // Transport-level failure (Wi-Fi drop, DNS timeout). Google was never reached, so the device
    // code is still valid; keep the sign-in alive unless the connection looks dead.
    ++auth_poll_failures_;
    if (auth_poll_failures_ >= kAuthMaxPollFailures) {
      google_auth_pending_ = false;
      device_code_ = {};
      set_status(StatusKind::Error, "Network trouble stopped the sign-in: " + response.error);
    } else {
      set_status(StatusKind::Info, "Network hiccup while checking with Google; retrying.");
    }
    return;
  }
  auth_poll_failures_ = 0;

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
    google_connected_ = true;
    google_auth_pending_ = false;
    device_code_ = {};
    if (!save_google_token_cache()) {
      set_status(StatusKind::Error, "Google connected, but token save failed.");
      return;
    }

    // The first Drive listing runs from the main loop on the next frame; doing it here would
    // keep the sign-in screen frozen for several more requests.
    pending_remote_refresh_ = true;
    set_status(StatusKind::Success, "Google Drive connected.");
    return;
  }

  if (token.error == "authorization_pending") {
    set_status(StatusKind::Info, "Waiting for approval on your phone.");
  } else if (token.error == "slow_down") {
    // RFC 8628: on slow_down the client must add five seconds to the poll interval.
    auth_poll_interval_seconds_ += 5;
    auth_poll_delay_frames_ = auth_poll_interval_seconds_ * kFramesPerSecond;
    set_status(StatusKind::Info, "Waiting for approval on your phone.");
  } else if (token.error == "expired_token" || token.error == "invalid_grant") {
    // Google does not send the RFC 8628 expired_token error; a device code that expired or was
    // already claimed comes back as invalid_grant instead.
    google_auth_pending_ = false;
    device_code_ = {};
    set_status(StatusKind::Error, "Sign-in code expired. Press Triangle for a new code.");
  } else if (token.error == "access_denied") {
    google_auth_pending_ = false;
    device_code_ = {};
    set_status(StatusKind::Error, "Google access was denied on the other device.");
  } else {
    google_auth_pending_ = false;
    device_code_ = {};
    set_status(StatusKind::Error, "Google auth failed: " + token_error_text(token));
  }
}

bool App::save_google_token_cache() {
  return write_text_file(kGoogleTokenPath, serialize_google_token_cache(google_token_cache_));
}

bool App::refresh_google_access_token() {
  if (!load_google_credentials() || google_token_cache_.refresh_token.empty()) {
    return false;
  }

  BusyLabelScope busy("Refreshing Google session");
  HttpClient http;
  const HttpResponse response = http.post_form(
      kGoogleTokenEndpoint,
      build_refresh_token_request_body(google_credentials_.client_id, google_credentials_.client_secret,
                                       google_token_cache_.refresh_token));
  const TokenResponse token = parse_token_response(response.body);
  if (!token.ok) {
    google_connected_ = false;
    drive_synced_ = false;
    if (token.error == "invalid_grant") {
      // The refresh token was revoked or expired server-side (for example a consent screen still
      // in "Testing" status expires refresh tokens after seven days). Clear the stored token so
      // the next launch does not claim a connection that no longer works.
      google_token_cache_ = {};
      save_google_token_cache();
      set_status(StatusKind::Error, "Google session expired. Press Triangle to reconnect.");
    } else {
      set_status(StatusKind::Error, "Google refresh failed: " + token_error_text(token));
    }
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
    set_status(StatusKind::Info, "Connect Google Drive first.");
    return false;
  }
  if (!google_token_cache_.access_token.empty() &&
      google_token_cache_.expires_at_epoch_seconds > current_epoch_seconds() + 60) {
    return true;
  }
  return refresh_google_access_token();
}

HttpResponse App::drive_request(const std::function<HttpResponse(const std::string &)> &send) {
  HttpResponse response = send(google_token_cache_.access_token);
  if (response.status == 401 && refresh_google_access_token()) {
    // Google can revoke an access token before its local expiry timestamp; one refresh-and-retry
    // covers that without turning every Drive call into a loop.
    response = send(google_token_cache_.access_token);
  }
  return response;
}

std::string App::find_drive_folder(const std::string &folder_name,
                                   const std::string &parent_id) {
  const std::string list_url = std::string(kDriveFilesEndpoint) + "?" +
                               build_drive_find_folder_query(folder_name, parent_id);
  const HttpResponse list_response = drive_request([&](const std::string &token) {
    return HttpClient().get_json(list_url, token);
  });
  if (list_response.ok) {
    const DriveFileList files = parse_drive_file_list(list_response.body);
    if (files.ok && !files.files.empty()) {
      return files.files[0].id;
    }
  }
  return {};
}

std::string App::find_or_create_drive_folder(const std::string &folder_name,
                                             const std::string &parent_id) {
  const std::string existing = find_drive_folder(folder_name, parent_id);
  if (!existing.empty()) {
    return existing;
  }

  const std::string create_url = std::string(kDriveFilesEndpoint) + "?fields=id%2Cname";
  const HttpResponse create_response = drive_request([&](const std::string &token) {
    return HttpClient().post_json(
        create_url, build_drive_folder_metadata_json(folder_name, parent_id), token);
  });
  if (!create_response.ok) {
    set_status(StatusKind::Error, "Drive folder create failed.");
    return {};
  }

  const DriveFileList created = parse_drive_file_list(create_response.body);
  if (!created.ok || created.files.empty()) {
    set_status(StatusKind::Error, "Drive folder response invalid.");
    return {};
  }
  return created.files[0].id;
}

void App::remove_drive_folder_if_empty(const std::string &folder_name) {
  const auto folder = drive_folder_ids_.find(folder_name);
  if (folder == drive_folder_ids_.end()) {
    return;
  }

  // Deleting a Drive folder permanently removes everything inside it, including files this app
  // cannot see under the drive.file scope. Only clean up after Drive confirms the folder is
  // empty, and treat any failure as harmless: an empty folder costs nothing and the next sync
  // simply ignores it.
  const std::string children_url =
      std::string(kDriveFilesEndpoint) + "?" + build_drive_list_children_query(folder->second);
  const HttpResponse children = drive_request([&](const std::string &token) {
    return HttpClient().get_json(children_url, token);
  });
  if (!children.ok) {
    return;
  }
  const DriveFileList list = parse_drive_file_list(children.body);
  if (!list.ok || !list.files.empty()) {
    return;
  }

  const std::string folder_url =
      std::string(kDriveFilesEndpoint) + "/" + form_url_encode(folder->second);
  const HttpResponse removed = drive_request([&](const std::string &token) {
    return HttpClient().delete_request(folder_url, token);
  });
  if (removed.ok) {
    drive_folder_ids_.erase(folder);
  }
}

bool App::sync_drive_index() {
  if (!ensure_google_access_token()) {
    return false;
  }
  BusyLabelScope busy("Syncing with Google Drive");

  // First sweep: every folder the app can see (drive.file scope limits this to folders Save
  // Keeper created). The root "PSV Saves" folder is identified by name; save folders are the
  // ones directly under it.
  std::vector<DriveFile> folders;
  std::string page_token;
  do {
    const std::string url = std::string(kDriveFilesEndpoint) + "?" +
                            build_drive_list_all_folders_query(page_token);
    const HttpResponse response = drive_request([&](const std::string &token) {
      return HttpClient().get_json(url, token);
    });
    if (!response.ok) {
      set_status(StatusKind::Error, "Drive folder listing failed.");
      return false;
    }
    const DriveFileList list = parse_drive_file_list(response.body);
    if (!list.ok) {
      set_status(StatusKind::Error, "Drive folder response invalid.");
      return false;
    }
    folders.insert(folders.end(), list.files.begin(), list.files.end());
    page_token = list.next_page_token;
  } while (!page_token.empty());

  std::string root_id = drive_root_folder_id_;
  for (const DriveFile &folder : folders) {
    if (folder.name == kGoogleDriveRootFolderName) {
      root_id = folder.id;
      break;
    }
  }

  drive_folder_ids_.clear();
  std::unordered_map<std::string, std::string> folder_id_to_name;
  for (const DriveFile &folder : folders) {
    if (!root_id.empty() && folder.parent_id == root_id) {
      drive_folder_ids_[folder.name] = folder.id;
      folder_id_to_name[folder.id] = folder.name;
    }
  }

  // Second sweep: every non-folder file, grouped under its save folder. Two paginated requests
  // total for typical libraries, instead of one round-trip per selected game.
  drive_index_.clear();
  page_token.clear();
  do {
    const std::string url = std::string(kDriveFilesEndpoint) + "?" +
                            build_drive_list_all_files_query(page_token);
    const HttpResponse response = drive_request([&](const std::string &token) {
      return HttpClient().get_json(url, token);
    });
    if (!response.ok) {
      set_status(StatusKind::Error, "Drive backup listing failed.");
      return false;
    }
    const DriveFileList list = parse_drive_file_list(response.body);
    if (!list.ok) {
      set_status(StatusKind::Error, "Drive backup response invalid.");
      return false;
    }
    for (const DriveFile &file : list.files) {
      if (file.name.size() < 4 || file.name.compare(file.name.size() - 4, 4, ".zip") != 0) {
        continue;
      }
      const auto folder = folder_id_to_name.find(file.parent_id);
      if (folder != folder_id_to_name.end()) {
        drive_index_[folder->second].push_back({file.name, file.id});
      }
    }
    page_token = list.next_page_token;
  } while (!page_token.empty());

  for (auto &entry : drive_index_) {
    // Timestamped names sort lexically; newest first matches the local backup list ordering.
    std::sort(entry.second.begin(), entry.second.end(),
              [](const RemoteBackup &a, const RemoteBackup &b) { return a.name > b.name; });
  }
  drive_root_folder_id_ = root_id;
  drive_synced_ = true;
  return true;
}

void App::refresh_remote_backups_view() {
  remote_backups_.clear();
  const SaveRecord *save = selected_save_record();
  if (save && google_connected_) {
    const std::string folder_name = resolved_drive_folder_name(save->id);
    const auto found =
        folder_name.empty() ? drive_index_.end() : drive_index_.find(folder_name);
    if (found != drive_index_.end()) {
      remote_backups_ = found->second;
    }
  }
  if (selected_backup_ > backup_count()) {
    selected_backup_ = 0;
  }
}

std::vector<std::string> App::remote_backup_names() const {
  std::vector<std::string> names;
  names.reserve(remote_backups_.size());
  for (const RemoteBackup &backup : remote_backups_) {
    names.push_back(backup.name);
  }
  return names;
}

std::size_t App::backup_count() const {
  return remote_backups_.size() + local_backups_.size();
}

bool App::selected_backup_is_remote() const {
  return selected_backup_ > 0 && selected_backup_ - 1 < remote_backups_.size();
}

std::string App::selected_backup_name() const {
  if (selected_backup_ == 0) {
    return {};
  }
  if (selected_backup_is_remote()) {
    return remote_backups_[selected_backup_ - 1].name;
  }
  if (local_backups_.empty()) {
    return {};
  }
  const std::size_t local_index = selected_backup_ - 1 - remote_backups_.size();
  return local_backups_[local_index % local_backups_.size()];
}

// Actual Drive folder holding this save's backups: either the bare key created by older versions
// or a "<key> <title>" folder; empty when Drive has none yet.
std::string App::resolved_drive_folder_name(const std::string &save_id) const {
  const std::string save_key = drive_folder_name_for(save_id);
  for (const auto &entry : drive_index_) {
    if (drive_folder_matches_save(entry.first, save_key)) {
      return entry.first;
    }
  }
  for (const auto &entry : drive_folder_ids_) {
    if (drive_folder_matches_save(entry.first, save_key)) {
      return entry.first;
    }
  }
  return {};
}

bool App::remote_backup_exists(const std::string &save_id, const std::string &backup_name) const {
  const std::string folder_name = resolved_drive_folder_name(save_id);
  if (folder_name.empty()) {
    return false;
  }
  const auto indexed = drive_index_.find(folder_name);
  if (indexed == drive_index_.end()) {
    return false;
  }
  for (const RemoteBackup &remote : indexed->second) {
    if (remote.name == backup_name) {
      return true;
    }
  }
  return false;
}

void App::handle_upload_button() {
  cancel_sync_all_confirmation();
  cancel_duplicate_backup_confirmation();
  const SaveRecord *selected = selected_save_record();
  if (!selected) {
    set_status(StatusKind::Info, "No save selected.");
    return;
  }
  if (selected_backup_ == 0 || selected_backup_is_remote() || local_backups_.empty()) {
    set_status(StatusKind::Info, "Select a local backup to upload.");
    return;
  }
  const std::string backup_name = selected_backup_name();

  // A snapshot never changes after creation, so a same-named file already in Drive is the same
  // backup; skip the upload instead of stacking duplicates (Drive allows same-name siblings).
  if (remote_backup_exists(selected->id, backup_name)) {
    set_status(StatusKind::Info, "This backup is already on Google Drive.");
    return;
  }

  if (!ensure_google_access_token()) {
    return;
  }

  BusyLabelScope busy("Uploading backup");
  if (!upload_local_backup(*selected, backup_name)) {
    return;
  }
  refresh_remote_backups_view();
  // The insert shifted every menu index below the new entry; focus the uploaded copy so the
  // selection stays on the file this action was about.
  for (std::size_t i = 0; i < remote_backups_.size(); ++i) {
    if (remote_backups_[i].name == backup_name) {
      selected_backup_ = 1 + i;
      break;
    }
  }
  set_status(StatusKind::Success, "Uploaded [GD] " + display_backup_name(backup_name));
}

// Sends one local archive to its save folder on Drive and slots it into the index. Error status
// is set here so both the single upload and the batch report the same failures.
bool App::upload_local_backup(const SaveRecord &save, const std::string &backup_name) {
  const std::string save_key = drive_folder_name_for(save.id);
  const std::string desired_name = drive_save_folder_name(save_key, save.display_name);

  if (drive_root_folder_id_.empty()) {
    drive_root_folder_id_ = find_or_create_drive_folder(kGoogleDriveRootFolderName, "root");
    if (drive_root_folder_id_.empty()) {
      return false;
    }
  }

  // Folder ids from the last index sync avoid lookup requests per upload. Without an index hit,
  // a bare-key folder from an older version is searched for before creating a titled one, so the
  // same save never splits across two Drive folders.
  std::string folder_name = resolved_drive_folder_name(save.id);
  std::string folder_id;
  if (!folder_name.empty()) {
    const auto cached_folder = drive_folder_ids_.find(folder_name);
    if (cached_folder != drive_folder_ids_.end()) {
      folder_id = cached_folder->second;
    }
  }
  if (folder_id.empty()) {
    folder_id = find_drive_folder(save_key, drive_root_folder_id_);
    if (!folder_id.empty()) {
      folder_name = save_key;
    } else {
      folder_id = find_or_create_drive_folder(desired_name, drive_root_folder_id_);
      if (folder_id.empty()) {
        return false;
      }
      folder_name = desired_name;
    }
    drive_folder_ids_[folder_name] = folder_id;
  }

  // Opportunistic upgrade: a bare-key folder gains the game title once it is known, so old
  // uploads become browsable on Drive too. A failed rename is harmless - the bare name keeps
  // matching by key prefix and the next upload retries.
  if (folder_name == save_key && desired_name != save_key) {
    const std::string rename_url = std::string(kDriveFilesEndpoint) + "/" +
                                   form_url_encode(folder_id) + "?fields=id%2Cname";
    const HttpResponse renamed = drive_request([&](const std::string &token) {
      return HttpClient().patch_json(rename_url, build_drive_rename_metadata_json(desired_name),
                                     token);
    });
    if (renamed.ok) {
      drive_folder_ids_.erase(folder_name);
      drive_folder_ids_[desired_name] = folder_id;
      const auto indexed = drive_index_.find(folder_name);
      if (indexed != drive_index_.end()) {
        drive_index_[desired_name] = std::move(indexed->second);
        drive_index_.erase(indexed);
      }
      folder_name = desired_name;
    }
  }

  const std::string archive_path = local_backup_archive_path(kBackupRoot, save.id, backup_name);
  const HttpResponse upload_response = drive_request([&](const std::string &token) {
    return HttpClient().post_multipart_file(
        kDriveUploadEndpoint, build_drive_upload_metadata_json(backup_name, folder_id),
        archive_path, token);
  });
  if (!upload_response.ok) {
    set_status(StatusKind::Error, "Drive upload failed.");
    return false;
  }

  // The upload response carries the new file's id; slotting it into the index directly keeps the
  // [GD] list current without another full sync.
  const DriveFileList uploaded = parse_drive_file_list(upload_response.body);
  if (uploaded.ok && !uploaded.files.empty()) {
    std::vector<RemoteBackup> &list = drive_index_[folder_name];
    list.push_back({uploaded.files[0].name, uploaded.files[0].id});
    std::sort(list.begin(), list.end(),
              [](const RemoteBackup &a, const RemoteBackup &b) { return a.name > b.name; });
  } else {
    sync_drive_index();
  }
  return true;
}

void App::cancel_sync_all_confirmation() {
  if (sync_all_confirmation_pending_) {
    sync_all_confirmation_pending_ = false;
    set_status(StatusKind::Info, "Backup & upload cancelled.");
  }
}

bool App::poll_batch_cancel() {
  if (!batch_running_) {
    return false;
  }
  if (!batch_cancel_requested_) {
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const unsigned int cancel_mask = enter_is_cross_ ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
    if ((pad.buttons & cancel_mask) != 0) {
      batch_cancel_requested_ = true;
    }
  }
  return batch_cancel_requested_;
}

void App::cancel_duplicate_backup_confirmation() {
  if (duplicate_backup_confirmation_pending_) {
    duplicate_backup_confirmation_pending_ = false;
    set_status(StatusKind::Info, "Backup cancelled.");
  }
}

void App::begin_sync_all() {
  restore_confirmation_pending_ = false;
  delete_confirmation_pending_ = false;
  duplicate_backup_confirmation_pending_ = false;
  sync_all_confirmation_pending_ = false;

  if (visible_saves_.empty()) {
    set_status(StatusKind::Info, "No saves in this tab.");
    return;
  }
  // A sign-in without internet cannot upload; the confirmation says so and the run only backs up.
  const bool drive_online = google_connected_ && HttpClient::network_reachable();
  // Accurate duplicate skipping needs the Drive index; a stored sign-in whose startup sync
  // failed gets one more chance here.
  if (drive_online && !drive_synced_) {
    sync_drive_index();
  }

  // Confirmation is instant: per-game work (signature check, zip, upload) is decided and done
  // during the run itself, so nothing is read twice and there is no scan phase to wait through.
  sync_all_confirmation_pending_ = true;
  set_status(StatusKind::Info,
             sync_all_confirm_message(visible_saves_.size(), save_category_label(category_),
                                      drive_online));
}

void App::run_sync_all() {
  sync_all_confirmation_pending_ = false;
  const std::vector<std::size_t> targets = visible_saves_;
  const std::size_t total = targets.size();
  SyncRunCounts run;
  bool auth_lost = false;
  const bool drive_online = google_connected_ && HttpClient::network_reachable();
  batch_running_ = true;
  batch_cancel_requested_ = false;

  for (std::size_t i = 0; i < total; ++i) {
    // Cancel lands between games (polled here) or mid-upload (polled by the HTTP cancel check);
    // a zip in flight always completes so no partial archive is left behind.
    if (poll_batch_cancel()) {
      run.games_left = total - i;
      break;
    }

    const SaveRecord &save = saves_[targets[i]];
    const std::string progress = " (" + std::to_string(i + 1) + "/" + std::to_string(total) + ")";
    ui_.set_batch_progress("Checking " + save.display_name + progress, i, total, enter_is_cross_);
    ui_.draw_busy("", 0, -1);

    const std::vector<std::string> backups = scan_local_backup_names(kBackupRoot, save.id);
    SyncItemInput input;
    input.drive_connected = drive_online;
    const std::vector<ArchiveEntryInfo> entries =
        compute_folder_entries(save.path, &input.entries_ok);
    input.folder_empty = entries.empty();
    if (input.entries_ok && !entries.empty()) {
      input.matches_existing = !matching_backup_name(entries, save.id, backups).empty();
    }
    if (!backups.empty()) {
      input.newest_local = backups[0];
      input.newest_on_drive = remote_backup_exists(save.id, backups[0]);
    }
    const SyncItemPlan plan = plan_sync_item(input);

    if (plan.unreadable) {
      ++run.failed;
      continue;
    }
    if (!plan.create_backup && !plan.will_upload) {
      ++run.up_to_date;
      continue;
    }

    std::string upload_name = plan.upload_existing;
    if (plan.create_backup) {
      ui_.set_batch_progress("Backing up " + save.display_name + progress, i, total,
                             enter_is_cross_);
      ui_.draw_busy("", 0, -1);
      const BackupResult result = create_backup_archive({
          save.path,
          kBackupRoot,
          save.id,
          current_backup_timestamp(),
      });
      if (!result.ok) {
        // The planned upload was this archive; there is nothing to send for this game.
        ++run.failed;
        continue;
      }
      ++run.backed_up;
      upload_name = archive_file_name(result.archive_path);
    }

    if (!plan.will_upload || upload_name.empty()) {
      continue;
    }
    if (auth_lost) {
      ++run.failed;
      continue;
    }
    if (!ensure_google_access_token()) {
      // A dead session would fail every remaining upload the same way; disable them instead of
      // sitting through one error per game.
      auth_lost = true;
      ++run.failed;
      continue;
    }
    ui_.set_batch_progress("Uploading " + save.display_name + progress, i, total,
                           enter_is_cross_);
    const std::string upload_label = "Uploading " + save.display_name + progress;
    BusyLabelScope busy(upload_label.c_str());
    if (upload_local_backup(save, upload_name)) {
      ++run.uploaded;
    } else if (batch_cancel_requested_) {
      // The failure was our own abort, not a network error; the game stays "left", its backup
      // (already counted) is safe locally and the next run will upload it.
      run.games_left = total - i;
      break;
    } else {
      ++run.failed;
    }
  }
  batch_running_ = false;
  batch_cancel_requested_ = false;
  ui_.clear_batch_progress();

  refresh_local_backups();
  refresh_remote_backups_view();
  if (run.uploaded > 0 && sort_mode_ == SaveSortMode::LastSynced) {
    apply_sort_and_rebuild();
  }
  std::string summary = sync_run_summary(run);
  if (auth_lost) {
    summary += " Google session expired.";
  }
  set_status(run.failed > 0 ? StatusKind::Error : StatusKind::Success, std::move(summary));
}

int App::run() {
  if (!ui_.initialize()) {
    return -1;
  }

  // Scan once at startup for the foundation build. Later actions that create, restore, or delete a
  // save will refresh this list explicitly so the UI does not rescan storage every frame.
  saves_ = scan_save_roots(default_save_roots());
  const AppDbMetadataResult metadata_result = apply_app_db_metadata(&saves_);
  if (!metadata_result.ok && !metadata_result.error.empty()) {
    set_status(StatusKind::Info, "Using save-folder metadata: " + metadata_result.error);
  }
  load_settings();
  apply_save_sort(&saves_, sort_mode_, {});
  // Open on the first tab that actually has saves so the app never starts on an empty grid.
  for (int i = 0; i < kSaveCategoryCount; ++i) {
    if (category_count(static_cast<SaveCategory>(i)) > 0) {
      category_ = static_cast<SaveCategory>(i);
      break;
    }
  }
  rebuild_visible_saves();
  refresh_local_backups();
  load_google_token_cache();

  // Bring the network stack up once for the whole run. Doing this per request was fragile: a
  // second initialization of an already-running stack fails and every request after that failed.
  std::string network_error;
  if (!HttpClient::network_startup(&network_error)) {
    set_status(StatusKind::Error, network_error);
  }
  network_connected_ = HttpClient::network_reachable();
  HttpClient::set_progress_hook([this](const std::string &label, long long done, long long total) {
    ui_.draw_busy(label, done, total);
  });
  HttpClient::set_cancel_check([this] { return poll_batch_cancel(); });

  // Follow the console's enter-button setting: western consoles confirm with Cross, Japanese
  // consoles with Circle. The primary Backup action sits on the confirm button and Cancel on the
  // other one, and the footer hints follow.
  SceAppUtilInitParam apputil_init_param {};
  SceAppUtilBootParam apputil_boot_param {};
  sceAppUtilInit(&apputil_init_param, &apputil_boot_param);
  int enter_button = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter_button);
  enter_is_cross_ = enter_button != SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE;
  const unsigned int backup_button = enter_is_cross_ ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
  const unsigned int cancel_button = enter_is_cross_ ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  // With a stored sign-in, load the Drive index right away so every game shows its [GD] entries
  // without a manual refresh; the progress overlay covers the wait.
  if (google_connected_) {
    if (sync_drive_index()) {
      refresh_remote_backups_view();
      if (sort_mode_ == SaveSortMode::LastSynced) {
        apply_sort_and_rebuild();
      }
    }
  }

  bool running = true;
  unsigned int previous_buttons = 0;
  unsigned int repeat_held_buttons = 0;
  int repeat_frames = 0;
  int rstick_direction_prev = 0;
  int rstick_frames = 0;
  int select_hold_frames = 0;
  bool select_hold_consumed = false;
  int network_poll_delay_frames = 0;
  while (running) {
    if (network_poll_delay_frames <= 0) {
      network_connected_ = HttpClient::network_reachable();
      network_poll_delay_frames = kFramesPerSecond;
    }
    --network_poll_delay_frames;

    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const unsigned int buttons = buttons_with_left_analog(pad);
    const unsigned int pressed =
        apply_hold_repeat(buttons, previous_buttons, &repeat_held_buttons, &repeat_frames);

    if ((pressed & SCE_CTRL_LEFT) != 0) {
      move_selected_save(-1);
    }
    if ((pressed & SCE_CTRL_RIGHT) != 0) {
      move_selected_save(1);
    }
    if ((pressed & SCE_CTRL_UP) != 0) {
      move_selected_save(-kSaveGridColumns);
    }
    if ((pressed & SCE_CTRL_DOWN) != 0) {
      move_selected_save(kSaveGridColumns);
    }
    if ((pressed & SCE_CTRL_LTRIGGER) != 0) {
      move_selected_category(-1);
    }
    if ((pressed & SCE_CTRL_RTRIGGER) != 0) {
      move_selected_category(1);
    }

    // The right stick browses the backup list with the same edge-plus-repeat feel as the buttons.
    int rstick_direction = 0;
    if (pad.ry < kAnalogCenter - kAnalogDeadZone) {
      rstick_direction = -1;
    } else if (pad.ry > kAnalogCenter + kAnalogDeadZone) {
      rstick_direction = 1;
    }
    if (rstick_direction != rstick_direction_prev) {
      rstick_frames = 0;
      if (rstick_direction != 0) {
        move_selected_backup(rstick_direction);
      }
    } else if (rstick_direction != 0) {
      ++rstick_frames;
      if (rstick_frames >= kRepeatInitialDelayFrames &&
          ((rstick_frames - kRepeatInitialDelayFrames) % kRepeatIntervalFrames) == 0) {
        move_selected_backup(rstick_direction);
      }
    }
    rstick_direction_prev = rstick_direction;
    if ((pressed & backup_button) != 0) {
      handle_action_button();
    }
    if ((pressed & cancel_button) != 0) {
      cancel_restore_confirmation();
      cancel_delete_confirmation();
      cancel_sync_all_confirmation();
      cancel_duplicate_backup_confirmation();
      cancel_google_auth();
    }
    if ((pressed & SCE_CTRL_TRIANGLE) != 0) {
      handle_google_button();
    }
    // Select distinguishes tap from hold: a tap uploads the focused backup, a one-second hold
    // starts the tab-wide backup & upload batch. Only a release within the tap window counts as
    // a tap - once the hold hint has appeared, releasing early is a no-op, so backing out of the
    // gesture can never fire an accidental upload.
    if ((buttons & SCE_CTRL_SELECT) != 0) {
      ++select_hold_frames;
      if (!select_hold_consumed && select_hold_frames >= kSelectHoldTriggerFrames) {
        select_hold_consumed = true;
        begin_sync_all();
      }
    } else {
      if (select_hold_frames > 0 && !select_hold_consumed &&
          select_hold_frames < kSelectHoldHintFrames) {
        handle_upload_button();
      }
      select_hold_frames = 0;
      select_hold_consumed = false;
    }
    if ((pressed & SCE_CTRL_START) != 0) {
      handle_delete_button();
    }
    if ((pressed & SCE_CTRL_SQUARE) != 0) {
      cycle_sort_mode();
    }

    UiState ui_state;
    ui_state.saves = &saves_;
    ui_state.visible_saves = &visible_saves_;
    ui_state.active_category = category_;
    ui_state.sort_mode = sort_mode_;
    for (int i = 0; i < kSaveCategoryCount; ++i) {
      ui_state.category_counts[i] = category_count(static_cast<SaveCategory>(i));
    }
    ui_state.selected_save = selected_save_;
    ui_state.remote_backups = remote_backup_names();
    ui_state.local_backups = &local_backups_;
    ui_state.selected_backup = selected_backup_;
    ui_state.restore_confirmation_pending = restore_confirmation_pending_;
    ui_state.delete_confirmation_pending = delete_confirmation_pending_;
    ui_state.sync_all_confirmation_pending = sync_all_confirmation_pending_;
    ui_state.duplicate_backup_confirmation_pending = duplicate_backup_confirmation_pending_;
    ui_state.enter_is_cross = enter_is_cross_;
    ui_state.google_connected = google_connected_;
    ui_state.drive_synced = drive_synced_;
    ui_state.network_connected = network_connected_;
    ui_state.google_auth_pending = google_auth_pending_;
    ui_state.google_verification_url = device_code_.verification_url;
    ui_state.google_user_code = device_code_.user_code;
    ui_state.auth_seconds_left =
        google_auth_pending_
            ? static_cast<int>(device_code_expires_at_ - current_epoch_seconds())
            : 0;
    ui_state.status_message = status_message_;
    ui_state.status_kind = status_kind_;
    // While Select is held past the tap window, the status line explains what is about to happen
    // and a gauge fills toward the trigger; this overlays the frame only, without touching the
    // stored status, so an early release restores whatever was there.
    if (!select_hold_consumed && select_hold_frames >= kSelectHoldHintFrames) {
      ui_state.select_hold_fraction =
          std::min(1.0f, static_cast<float>(select_hold_frames) /
                             static_cast<float>(kSelectHoldTriggerFrames));
      ui_state.status_message = sync_all_hold_message(save_category_label(category_));
      ui_state.status_kind = StatusKind::Info;
    }
    ui_.draw(ui_state);
    previous_buttons = buttons;

    // Poll after drawing so the frame on screen already shows the waiting state while the
    // blocking token request runs.
    update_google_auth();

    if (pending_remote_refresh_ && !google_auth_pending_) {
      pending_remote_refresh_ = false;
      if (sync_drive_index()) {
        refresh_remote_backups_view();
        if (sort_mode_ == SaveSortMode::LastSynced) {
          apply_sort_and_rebuild();
        }
        set_status(StatusKind::Success, "Google Drive connected.");
      }
    }

    // This keeps the placeholder loop from busy-spinning. Vita2d swaps on vblank when configured,
    // but the small delay also keeps CPU use reasonable if vblank wait is disabled by a future build.
    sceKernelDelayThread(kFrameDelayUs);
  }

  HttpClient::network_shutdown();
  ui_.shutdown();
  sceKernelExitProcess(0);
  return 0;
}

} // namespace vsm::vita
