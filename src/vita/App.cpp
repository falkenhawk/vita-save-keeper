#include "vita/App.hpp"

#include "core/AppSettings.hpp"
#include "core/BackupArchive.hpp"
#include "core/BackupName.hpp"
#include "core/BackupStore.hpp"
#include "core/GoogleAuth.hpp"
#include "core/GoogleConfig.hpp"
#include "core/GoogleDrive.hpp"
#include "core/InputGesture.hpp"
#include "core/PathUtil.hpp"
#include "core/SaveScanner.hpp"
#include "core/Selection.hpp"
#include "vita/SaveAppDbMetadata.hpp"
#include "vita/mount/user/save_data_mount.h"
#include "vita/net/HttpClient.hpp"

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/system_param.h>
#include <taihen.h>
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
constexpr const char *kSaveTimeCachePath = "ux0:data/save-keeper/save-times.json";
constexpr const char *kMountKernelPath =
    "ux0:app/SVK000001/sce_sys/save-data-kernel.skprx";
constexpr const char *kMountUserPath = "ux0:app/SVK000001/sce_sys/save-data-user.suprx";
// download_remote_backup_metadata's "Drive has no sidecar for this archive" outcome;
// open_save_details keys the Cloud-only explanation off this exact value.
constexpr const char *kNoRemoteSidecarError = "no details file in the Cloud";
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
// Select and Square both split tap from hold: a quick tap does the tap action (Select uploads or
// downloads, Square cycles the sort), a one-second hold does the hold action (Select's batch,
// Square's label editor). kSelectHoldTapFrames is the tap window - release within it fires the
// tap - and also when the hold gauge appears. Releasing after the gauge shows (between the tap
// window and the trigger) is a deliberate back-out and does nothing, so a mistimed hold never
// fires the tap action by surprise. ~400 ms is forgiving enough for a normal tap while still
// leaving room to abort a hold.
constexpr int kSelectHoldTriggerFrames = 60;
constexpr int kSelectHoldTapFrames = 24;
// Frames the focused save must stay put before its mount-only time is read (~150 ms at 60 fps).
// Long enough that scrolling through encrypted saves does not mount every one it passes.
constexpr int kSaveTimeResolveDelayFrames = 10;
constexpr std::size_t kMaxSdslotFileSize =
    kSdslotHeaderSize + kMaxSaveSlots * kSdslotRecordSize;

std::vector<SaveRoot> default_save_roots() {
  return {
      {SavePlatform::Vita, "ux0:user/00/savedata"},
      {SavePlatform::GameCard, "grw0:savedata"},
      {SavePlatform::Psp, "ux0:pspemu/PSP/SAVEDATA"},
  };
}

BackupTimestamp backup_timestamp_from(const SaveDateTime &value) {
  return {value.year, value.month, value.day, value.hour, value.minute, value.second};
}

SaveMetadata resolve_live_save_metadata(const std::string &save_path,
                                        const SaveDateTime &backup_clock, bool allow_pfs_mount,
                                        bool bridge_available) {
  // PSP and other plain save folders must never be handed to AppMgr's PFS mount routine: mounting
  // a plain folder creates sce_pfs bookkeeping in the user's save. If PFS metadata is absent, its
  // ordinary file times are already the authoritative information we need.
  if (!allow_pfs_mount || !save_directory_has_pfs_metadata(save_path)) {
    return resolve_save_metadata(save_path, backup_clock);
  }

  char mount_point[16] {};
  char key[16] {};
  SaveKeeperMountArgs args {};
  args.process_title_id = "SVK000001";
  args.path = save_path.c_str();
  args.key = key;
  args.mount_point = mount_point;

  // Retail Vita saves are PFS-encrypted on disk. Try the mount IDs VitaShell knows, then retain
  // AppMgr's public read-only fallback for unusual setups. A successful mount decrypts the same
  // save path; the returned name is used only to unmount immediately after reading metadata.
  int mount_result = -1;
  if (bridge_available) {
    // Only call the kernel bridge syscall when its modules actually loaded. Otherwise skip
    // straight to the AppMgr fallback rather than invoke an unresolved weak import.
    static constexpr int kSavedataMountIds[] = {0x6E, 0x12E, 0x12F, 0x3ED};
    for (const int id : kSavedataMountIds) {
      args.id = id;
      mount_result = saveKeeperUserMountById(&args);
      if (mount_result >= 0) {
        break;
      }
    }
  }
  if (mount_result < 0) {
    mount_result = sceAppMgrGameDataMount(save_path.c_str(), nullptr, nullptr, mount_point);
  }
  // Whether or not the mount succeeded, resolve_save_metadata falls back to the newest file's
  // modification time when there are no readable Vita slots. Mounting decrypts file *contents*, not
  // their timestamps, so that time is the same approximate "last written" moment either way - a
  // useful fallback for saves whose mount fails, rather than showing nothing.
  SaveMetadata metadata = resolve_save_metadata(save_path, backup_clock);
  if (mount_result >= 0) {
    sceAppMgrUmount(mount_point);
  }
  return metadata;
}

class BackupInspectionDirectory {
public:
  explicit BackupInspectionDirectory(std::string path) : path_(std::move(path)) {
    // A previous crash may have left this private work directory behind. It is never a backup or
    // a live save, so clearing it before reuse is safe.
    remove_backup_inspection_directory(path_);
  }

  ~BackupInspectionDirectory() {
    // resolve_live_save_metadata unmounts before returning, so encrypted files are no longer in
    // use when this cleanup runs.
    remove_backup_inspection_directory(path_);
  }

  const std::string &path() const { return path_; }

private:
  std::string path_;
};

bool initialize_save_data_mount_bridge() {
  // Kernel modules survive an app restart on some setups. "Already loaded" is therefore usable;
  // the process-local user bridge is still loaded on every launch.
  const SceUID kernel_module = taiLoadStartKernelModule(kMountKernelPath, 0, nullptr, 0);
  if (kernel_module < 0 && static_cast<unsigned int>(kernel_module) != 0x8002D013U) {
    return false;
  }
  return sceKernelLoadStartModule(kMountUserPath, 0, nullptr, 0, nullptr, nullptr) >= 0;
}

long long current_epoch_seconds() {
  return static_cast<long long>(std::time(nullptr));
}

bool upgrade_legacy_metadata_file(const std::string &path, const std::string &identity,
                                  SaveMetadataJsonResult *metadata) {
  if (!metadata || !metadata->ok || metadata->schema_version >= kSaveMetadataJsonVersion) {
    return true;
  }
  std::string error;
  if (!write_save_metadata_json_atomic(path, identity, metadata->metadata, &error)) {
    metadata->ok = false;
    metadata->error = error;
    return false;
  }
  metadata->schema_version = kSaveMetadataJsonVersion;
  return true;
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

LocalSnapshotResult App::create_local_snapshot(const SaveRecord &save,
                                               const std::string &suffix,
                                               bool report_progress,
                                               bool force_new) {
  LocalSnapshotResult snapshot;
  // Resolve once so the ZIP name and JSON describe the same moment, even if creating the archive
  // takes long enough for the wall clock to tick over.
  const SaveMetadata metadata = resolve_live_save_metadata(
      save.path, current_local_datetime(), save.platform != SavePlatform::Psp,
      mount_bridge_ready_);

  bool entries_ok = false;
  const std::vector<ArchiveEntryInfo> entries = compute_folder_entries(save.path, &entries_ok);
  if (!entries_ok) {
    snapshot.error = "could not read the save folder";
    return snapshot;
  }
  if (entries.empty()) {
    snapshot.error = "save folder is empty";
    return snapshot;
  }

  const std::vector<std::string> local_names = scan_local_backup_names(kBackupRoot, save.id);
  std::vector<std::string> remote_names;
  const std::string folder_name = resolved_drive_folder_name(save.id);
  const auto indexed = drive_index_.find(folder_name);
  if (indexed != drive_index_.end()) {
    remote_names.reserve(indexed->second.size());
    for (const RemoteBackup &remote : indexed->second) {
      remote_names.push_back(remote.name);
    }
  }

  const BackupTimestamp timestamp = backup_timestamp_from(metadata.saved_at);
  const BackupCreationPlan plan =
      plan_backup_creation(timestamp, suffix, entries, kBackupRoot, save.id, local_names,
                           remote_names, !force_new);
  snapshot.archive_name = plan.archive_name;
  snapshot.reused = plan.reuse_existing;

  // Matching content at this exact save-time identity is already safely backed up. Reuse it;
  // otherwise create the pre-allocated name exclusively so a collision can never overwrite it.
  if (!plan.reuse_existing) {
    BackupRequest request;
    request.source_path = save.path;
    request.backup_root = kBackupRoot;
    request.save_id = save.id;
    request.timestamp = timestamp;
    request.archive_name = plan.archive_name;
    if (report_progress) {
      request.progress = [this](std::uint64_t done, std::uint64_t total) {
        ui_.draw_busy("Creating backup", static_cast<long long>(done),
                      static_cast<long long>(total));
      };
    }
    const BackupResult backup = create_backup_archive(request);
    if (!backup.ok) {
      snapshot.error = backup.error;
      return snapshot;
    }
  }

  std::string metadata_error;
  const std::string metadata_path =
      local_backup_metadata_path(kBackupRoot, save.id, plan.archive_name);
  if (!write_save_metadata_json_atomic(metadata_path, backup_identity(plan.archive_name),
                                       metadata, &metadata_error)) {
    // The archive remains a valid whole-save backup. Details can be recovered from it later when
    // sdslot.dat is present, so report a warning rather than deleting the ZIP.
    snapshot.metadata_warning = true;
    snapshot.error = metadata_error;
  }
  snapshot.ok = true;
  return snapshot;
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

bool App::resolve_save_time(SaveRecord *save) {
  if (!save || !save->save_time_requires_mount) {
    return save && save->save_time_known;
  }
  const SaveMetadata metadata =
      resolve_live_save_metadata(save->path, {}, true, mount_bridge_ready_);
  const bool resolved = apply_mounted_save_time(save, metadata);
  // Cache every outcome reached with a healthy mount bridge - an exact slot time, a filesystem
  // fallback (games with no slot table), and even "no readable time at all" - so none of them is
  // re-derived every launch. Skip caching when the bridge is down: that is the one failure that
  // can heal without the save folder changing, so those saves must retry next launch.
  if (mount_bridge_ready_) {
    // Fingerprint after the mount, so bookkeeping the mount itself touched is part of the stored
    // state and the next scan sees an unchanged folder.
    save->fingerprint = compute_save_fingerprint(save->path);
    if (save->fingerprint.ok) {
      save_time_cache_.entries[save->id] = {save->fingerprint, resolved, save->saved_at};
      save_time_cache_dirty_ = true;
    }
  }
  return resolved;
}

void App::apply_save_time_cache() {
  for (SaveRecord &save : saves_) {
    if (!save.save_time_requires_mount || !save.fingerprint.ok) {
      continue;
    }
    const auto entry = save_time_cache_.entries.find(save.id);
    if (entry == save_time_cache_.entries.end() ||
        !entry->second.fingerprint.matches(save.fingerprint)) {
      continue;
    }
    save.save_time_requires_mount = false;
    if (entry->second.has_time) {
      save.saved_at = entry->second.saved_at;
      save.saved_at_epoch = save_datetime_to_local_epoch(entry->second.saved_at);
      save.save_time_known = true;
    } else {
      // Cached "no readable time": show unknown without re-mounting an unchanged save.
      save.save_time_known = false;
    }
  }
}

void App::flush_save_time_cache() {
  if (!save_time_cache_dirty_) {
    return;
  }
  // Best effort: a failed write only means these times are read through a mount again next launch.
  std::string error;
  if (write_save_time_cache_atomic(kSaveTimeCachePath, save_time_cache_, &error)) {
    save_time_cache_dirty_ = false;
  }
}

void App::schedule_selected_save_time_resolve() {
  // Defer the (blocking) mount until the selection settles, so scrolling past several encrypted
  // saves does not mount each one in turn. The main loop counts this down and then resolves
  // without a modal, leaving the grid on screen during the brief read.
  if (visible_saves_.empty()) {
    pending_time_resolve_frames_ = -1;
    return;
  }
  const SaveRecord &save = saves_[visible_saves_[selected_save_ % visible_saves_.size()]];
  pending_time_resolve_frames_ =
      save.save_time_requires_mount ? kSaveTimeResolveDelayFrames : -1;
}

void App::resolve_selected_save_time() {
  if (visible_saves_.empty()) {
    return;
  }
  SaveRecord &save = saves_[visible_saves_[selected_save_ % visible_saves_.size()]];
  if (!save.save_time_requires_mount) {
    return;
  }
  // Synchronous mount on the main thread. Deliberately not on a background thread: mounting a save
  // from a second thread leaked AppMgr mount slots until, mid-session, the system refused any
  // further mounts. The grid freezes briefly during the read, which the debounce keeps to only the
  // save the user settles on.
  resolve_save_time(&save);
  flush_save_time_cache();
}

bool App::resolve_all_save_times() {
  const std::size_t total = static_cast<std::size_t>(std::count_if(
      saves_.begin(), saves_.end(),
      [](const SaveRecord &save) { return save.save_time_requires_mount; }));
  if (total == 0) {
    return true;
  }
  constexpr const char *kContext = "Switching to Last Saved sort";
  constexpr const char *kCancelHint = "cancel and sort by name";

  // Only controller samples newer than this moment count as a cancel. The Square tap that switched
  // the sort mode is still in the sampling history and must not cancel the read it just started.
  SceCtrlData armed {};
  sceCtrlPeekBufferPositive(0, &armed, 1);

  // Each save's resolve blocks this thread on a mount, so an instantaneous poll would only catch a
  // press timed exactly between two saves. Scanning the buffered sample history (up to 64 frames)
  // catches a quick tap made while a mount was running as soon as that mount returns.
  const auto cancel_requested = [&armed]() {
    SceCtrlData pads[64] = {};
    const int count = sceCtrlPeekBufferPositive(0, pads, 64);
    for (int i = 0; i < count; ++i) {
      if (pads[i].timeStamp > armed.timeStamp && (pads[i].buttons & SCE_CTRL_SQUARE) != 0) {
        return true;
      }
    }
    return false;
  };

  std::size_t done = 0;
  for (SaveRecord &save : saves_) {
    if (!save.save_time_requires_mount) {
      continue;
    }
    if (cancel_requested()) {
      // Swallow the rest of the press before returning: the main loop cycles the sort on a Square
      // release edge, so handing it a still-held Square would fire Name -> Backup immediately.
      SceCtrlData pad {};
      do {
        ui_.draw_busy("Reading save times", static_cast<long long>(done),
                      static_cast<long long>(total), kContext, kCancelHint);
        sceKernelDelayThread(16 * 1000);
        sceCtrlPeekBufferPositive(0, &pad, 1);
      } while ((pad.buttons & SCE_CTRL_SQUARE) != 0);
      // The times read before the cancel are still valid; keep them for the next launch.
      flush_save_time_cache();
      return false;
    }
    ui_.draw_busy("Reading save times", static_cast<long long>(done),
                  static_cast<long long>(total), kContext, kCancelHint);
    resolve_save_time(&save);
    ++done;
  }
  flush_save_time_cache();
  return true;
}

void App::apply_sort_and_rebuild() {
  std::string focused_id;
  if (const SaveRecord *current = selected_save_record()) {
    focused_id = current->id;
  }

  if (save_sort_requires_all_times(sort_mode_) && !resolve_all_save_times()) {
    // The user canceled the read; keep the name order rather than a half-resolved Last Saved order.
    sort_mode_ = SaveSortMode::Name;
    save_settings();
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
  schedule_selected_save_time_resolve();
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
  case SaveSortMode::LastBackup:
    set_status(StatusKind::Info, "Sorted by latest backup.");
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
    rebuild_backup_rows();
    return;
  }

  local_backups_ = scan_local_backup_names(kBackupRoot, save->id);
  rebuild_backup_rows();
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
    schedule_selected_save_time_resolve();
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
    schedule_selected_save_time_resolve();
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
    details_open_pending_ = false;
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
    set_status(StatusKind::Info, "Restore canceled.");
  }
}

void App::cancel_delete_confirmation() {
  if (delete_confirmation_pending_ || delete_scope_prompt_pending_) {
    delete_confirmation_pending_ = false;
    delete_scope_prompt_pending_ = false;
    set_status(StatusKind::Info, "Delete canceled.");
  }
}

void App::create_new_backup() {
  restore_confirmation_pending_ = false;
  delete_confirmation_pending_ = false;
  delete_scope_prompt_pending_ = false;
  const SaveRecord *selected = selected_save_record();
  if (!selected) {
    set_status(StatusKind::Info, "No save selected.");
    return;
  }

  const SaveRecord &save = *selected;
  const bool force_new = duplicate_backup_confirmation_pending_;
  if (!force_new) {
    // Content identical to an existing archive would only stack a same-bytes snapshot under a new
    // timestamp; warn first, and let a second press force it anyway (the batch never forces).
    ui_.draw_busy("Checking current save", 0, -1);
    bool signature_ok = false;
    const std::vector<ArchiveEntryInfo> entries = compute_folder_entries(save.path, &signature_ok);
    if (signature_ok && !entries.empty()) {
      const std::string match = matching_backup_name(entries, save.id, local_backups_);
      if (!match.empty()) {
        duplicate_backup_confirmation_pending_ = true;
        // Says why a new backup is redundant; the footer offers "Create New Backup Anyway".
        set_status(StatusKind::Info,
                   ui_.compose_status_with_name("No changes since ", display_backup_name(match),
                                                "."));
        return;
      }
    }
  }
  duplicate_backup_confirmation_pending_ = false;

  // One busy frame before the blocking ZIP work, so the screen does not look frozen.
  ui_.draw_busy("Creating backup", 0, -1);
  const LocalSnapshotResult result = create_local_snapshot(save, "", true, force_new);
  if (result.ok) {
    refresh_local_backups();
    // Focus the fresh (or safely reused) snapshot so an immediate Select-to-upload needs no
    // scrolling.
    const std::string &file_name = result.archive_name;
    focus_backup_row_by_identity(file_name);
    if (result.metadata_warning) {
      set_status(StatusKind::Info, "Backup created, but slot details could not be saved.");
      return;
    }
    if (result.reused) {
      clear_status();
      return;
    }
    // With Drive available, nudge the natural next step. The fresh snapshot is focused, so the
    // timestamp name would only repeat what the highlighted row already shows - and without it
    // the nudge always fits the status line untruncated.
    if (google_connected_ && network_connected_) {
      set_status(StatusKind::Success, "Backup created. Press Select to upload it.");
    } else {
      set_status(StatusKind::Success,
                 ui_.compose_status_with_name("Created ", display_backup_name(file_name), "."));
    }
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
  const BackupRow *row = selected_backup_row();
  if (!row) {
    set_status(StatusKind::Info, "Select a backup to delete.");
    return;
  }

  if (delete_scope_prompt_pending_) {
    // Second Start press with the scope prompt open means "everywhere".
    delete_scope_prompt_pending_ = false;
    perform_scoped_delete(true, true);
    return;
  }

  const std::string display = row->display_name();
  if (row->has_local() && row->has_remote()) {
    restore_confirmation_pending_ = false;
    delete_confirmation_pending_ = false;
    delete_scope_prompt_pending_ = true;
    // The footer's three scope buttons say where, so the status only needs to name what. Just
    // the quoted name ellipsizes, so the "?" always survives.
    set_status(StatusKind::Info, ui_.compose_status_with_name("Delete ", display, "?"));
    return;
  }

  if (!delete_confirmation_pending_) {
    restore_confirmation_pending_ = false;
    delete_confirmation_pending_ = true;
    // Card-only is the plain case, so the prompt just names what; a Cloud-only delete keeps its
    // "from the Cloud?" qualifier since removing the only remote copy is the weightier action.
    set_status(StatusKind::Info,
               ui_.compose_status_with_name("Delete ", display,
                                            row->has_remote() ? " from the Cloud?" : "?"));
    return;
  }
  delete_confirmation_pending_ = false;
  perform_scoped_delete(row->has_local(), row->has_remote());
}

void App::perform_scoped_delete(bool delete_local, bool delete_remote) {
  const SaveRecord *selected = selected_save_record();
  const BackupRow *selected_row = selected_backup_row();
  if (!selected || !selected_row) {
    return;
  }
  // The refreshes below rebuild backup_rows_ and invalidate the pointer.
  const BackupRow row = *selected_row;
  const std::string display = row.display_name();

  // Drive first: if the Drive delete fails, the card copy is untouched and the pair is intact,
  // instead of a half-deleted backup surviving only in the cloud.
  const bool remote_requested = delete_remote && row.has_remote();
  if (remote_requested) {
    if (!ensure_google_access_token()) {
      return;
    }
    const std::string file_id = remote_file_id_for(row.remote_name);
    if (file_id.empty()) {
      set_status(StatusKind::Error, "Cloud copy not found; refresh and retry.");
      return;
    }
    const std::string folder_name = resolved_drive_folder_name(selected->id);
    const auto folder = drive_folder_ids_.find(folder_name);
    const std::string folder_id = folder == drive_folder_ids_.end() ? "" : folder->second;
    // A both-sides delete does not distinguish which copy is going first (both are), so the modal
    // just names the backup; a Cloud-only delete says it is the Cloud copy. Name-only truncation
    // keeps the "Cloud backup" suffix intact for a long labeled name.
    const std::string busy_label =
        delete_local ? ui_.compose_modal_label("Deleting ", display, "")
                     : ui_.compose_modal_label("Deleting ", display, " Cloud backup");
    BusyLabelScope busy(busy_label.c_str());
    // Remove the optional companion first. If that request fails, keep the ZIP and report the
    // delete failure, rather than deleting the ZIP and stranding an orphaned JSON file. Should
    // the ZIP request then fail, its metadata can be rebuilt from the archive on the next visit.
    if (!folder_id.empty()) {
      const DriveFile sidecar = find_remote_sidecar(folder_id, file_id, row.remote_name);
      if (!sidecar.id.empty()) {
        const std::string sidecar_url =
            std::string(kDriveFilesEndpoint) + "/" + form_url_encode(sidecar.id);
        const HttpResponse sidecar_response = drive_request([&](const std::string &token) {
          return HttpClient().delete_request(sidecar_url, token);
        });
        if (!sidecar_response.ok) {
          set_status(StatusKind::Error, "Cloud delete failed.");
          return;
        }
      }
    }
    const std::string url = std::string(kDriveFilesEndpoint) + "/" + form_url_encode(file_id);
    const HttpResponse response = drive_request([&](const std::string &token) {
      return HttpClient().delete_request(url, token);
    });
    if (!response.ok) {
      set_status(StatusKind::Error, "Cloud delete failed.");
      return;
    }
    const auto indexed = drive_index_.find(folder_name);
    if (indexed != drive_index_.end()) {
      std::vector<RemoteBackup> &list = indexed->second;
      for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i].file_id == file_id) {
          list.erase(list.begin() + static_cast<long>(i));
          break;
        }
      }
      if (list.empty()) {
        drive_index_.erase(indexed);
      }
    }
    if (drive_index_.find(folder_name) == drive_index_.end()) {
      remove_drive_folder_if_empty(folder_name);
    }
    refresh_remote_backups_view();
  }

  bool local_failed = false;
  if (delete_local && row.has_local()) {
    const std::string archive_path =
        local_backup_archive_path(kBackupRoot, selected->id, row.local_name);
    local_failed = std::remove(archive_path.c_str()) != 0;
    refresh_local_backups();
  }
  const bool local_remains = row.has_local() && (!delete_local || local_failed);
  const bool remote_remains = row.has_remote() && !remote_requested;
  if (!local_remains && !remote_remains) {
    const std::string metadata_name = row.has_local() ? row.local_name : row.remote_name;
    const std::string metadata_path =
        local_backup_metadata_path(kBackupRoot, selected->id, metadata_name);
    std::remove(metadata_path.c_str());
  }
  if (selected_backup_ > backup_count()) {
    selected_backup_ = backup_count();
  }

  if (local_failed) {
    set_status(StatusKind::Error,
               remote_requested ? "Deleted the Cloud copy, but not the card copy."
                                : ui_.compose_status_with_name("Could not delete ", display, "."));
    return;
  }
  if (delete_local && remote_requested) {
    set_status(StatusKind::Success, ui_.compose_status_with_name("Deleted ", display, "."));
  } else if (remote_requested) {
    set_status(StatusKind::Success,
               ui_.compose_status_with_name("Deleted ", display, " from the Cloud."));
  } else {
    set_status(StatusKind::Success, ui_.compose_status_with_name("Deleted ", display, "."));
  }
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
  const BackupRow *selected_row = selected_backup_row();
  if (!selected || !selected_row) {
    return;
  }
  // Refreshes below rebuild backup_rows_; keep a stable copy of the row being restored.
  const BackupRow row = *selected_row;
  const std::string backup_name = row.primary_name();
  if (!restore_confirmation_pending_) {
    delete_confirmation_pending_ = false;
    delete_scope_prompt_pending_ = false;
    restore_confirmation_pending_ = true;
    set_status(StatusKind::Info, ui_.compose_status_with_name("Restore ", row.display_name(), "?"));
    return;
  }

  const SaveRecord &save = *selected;
  // A card copy restores directly; a cloud-only snapshot downloads into the local backup folder
  // first (and stays there, so the row becomes card + Drive).
  std::string archive_path = local_backup_archive_path(kBackupRoot, save.id, backup_name);
  const bool remote_restore = !row.has_local();

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
      const LocalSnapshotResult auto_result = create_local_snapshot(save, " auto", false);
      if (!auto_result.ok) {
        // Losing the current save is the one outcome this feature exists to prevent; a restore
        // does not proceed over a failed safety snapshot.
        restore_confirmation_pending_ = false;
        set_status(StatusKind::Error, "Could not back up current save: " + auto_result.error);
        return;
      }
      refresh_local_backups();
      // The new snapshot shifted the rows; re-locate the entry being restored.
      focus_backup_row_by_identity(backup_name);
    }
  }

  if (remote_restore) {
    if (!ensure_google_access_token()) {
      restore_confirmation_pending_ = false;
      return;
    }
    const std::string file_id = remote_file_id_for(row.remote_name);
    if (file_id.empty()) {
      set_status(StatusKind::Error, "Cloud copy not found; refresh and retry.");
      restore_confirmation_pending_ = false;
      return;
    }
    if (!ensure_parent_directory(archive_path)) {
      set_status(StatusKind::Error, "Could not create local backup folder.");
      restore_confirmation_pending_ = false;
      return;
    }
    const std::string busy_label =
        ui_.compose_modal_label("Downloading ", display_backup_name(backup_name), "");
    BusyLabelScope busy(busy_label.c_str());
    const std::string download_url = std::string(kDriveFilesEndpoint) + "/" +
                                     form_url_encode(file_id) + "?alt=media";
    const HttpResponse download = drive_request([&](const std::string &token) {
      return HttpClient().download_file(download_url, archive_path, token);
    });
    if (!download.ok) {
      // A failed stream leaves a partial zip that would list as a real backup on next refresh.
      std::remove(archive_path.c_str());
      restore_confirmation_pending_ = false;
      set_status(StatusKind::Error, "Cloud download failed.");
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
    // The live folder now holds different content; drop the cached time and re-read so the grid
    // does not keep showing the pre-restore save time.
    invalidate_save_time(save);
    schedule_selected_save_time_resolve();
    set_status(StatusKind::Success,
               ui_.compose_status_with_name(
                   remote_restore ? "Downloaded and restored " : "Restored ",
                   display_backup_name(backup_name), "."));
  } else {
    set_status(StatusKind::Error, "Restore failed: " + result.error);
  }
}

void App::invalidate_save_time(const SaveRecord &restored) {
  save_time_cache_.entries.erase(restored.id);
  save_time_cache_dirty_ = true;
  flush_save_time_cache();
  for (SaveRecord &record : saves_) {
    if (record.id != restored.id || record.platform != restored.platform) {
      continue;
    }
    record.save_time_requires_mount =
        record.platform != SavePlatform::Psp && save_directory_has_pfs_metadata(record.path);
    if (record.save_time_requires_mount) {
      // Encrypted again after the restore: the focused-save debounce (scheduled by the caller)
      // re-reads it through a mount and refills the cache.
      record.save_time_known = false;
      record.fingerprint = compute_save_fingerprint(record.path);
    } else {
      const SaveMetadata metadata = resolve_save_metadata(record.path, current_local_datetime());
      record.saved_at = metadata.saved_at;
      record.saved_at_epoch = save_datetime_to_local_epoch(metadata.saved_at);
      record.save_time_known = metadata.source != SaveTimeSource::BackupClock;
    }
    break;
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
      if (sort_mode_ == SaveSortMode::LastBackup) {
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

  BusyLabelScope busy("Contacting Google", /*indeterminate=*/true);
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
  // The dedicated sign-in panel already shows the QR instructions and waiting state. Keeping the
  // shared status line empty avoids repeating the same sentence in a narrower, truncated area.
  clear_status();
}

void App::cancel_google_auth() {
  if (!google_auth_pending_) {
    return;
  }
  google_auth_pending_ = false;
  device_code_ = {};
  set_status(StatusKind::Info, "Google sign-in canceled.");
}

void App::update_google_auth() {
  if (!google_auth_pending_) {
    return;
  }

  if (current_epoch_seconds() >= device_code_expires_at_) {
    google_auth_pending_ = false;
    device_code_ = {};
    set_status(StatusKind::Error, "Sign-in expired. Press Triangle to retry.");
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
      set_status(StatusKind::Info, "Network hiccup; retrying.");
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
    clear_status();
  } else if (token.error == "slow_down") {
    // RFC 8628: on slow_down the client must add five seconds to the poll interval.
    auth_poll_interval_seconds_ += 5;
    auth_poll_delay_frames_ = auth_poll_interval_seconds_ * kFramesPerSecond;
    clear_status();
  } else if (token.error == "expired_token" || token.error == "invalid_grant") {
    // Google does not send the RFC 8628 expired_token error; a device code that expired or was
    // already claimed comes back as invalid_grant instead.
    google_auth_pending_ = false;
    device_code_ = {};
    set_status(StatusKind::Error, "Sign-in expired. Press Triangle to retry.");
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

  BusyLabelScope busy("Refreshing Google session", /*indeterminate=*/true);
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
    set_status(StatusKind::Error, "Cloud folder create failed.");
    return {};
  }

  const DriveFileList created = parse_drive_file_list(create_response.body);
  if (!created.ok || created.files.empty()) {
    set_status(StatusKind::Error, "Cloud folder response invalid.");
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
  BusyLabelScope busy("Syncing with Google Drive", /*indeterminate=*/true);

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
      set_status(StatusKind::Error, "Cloud folder listing failed.");
      return false;
    }
    const DriveFileList list = parse_drive_file_list(response.body);
    if (!list.ok) {
      set_status(StatusKind::Error, "Cloud folder response invalid.");
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
      set_status(StatusKind::Error, "Cloud backup listing failed.");
      return false;
    }
    const DriveFileList list = parse_drive_file_list(response.body);
    if (!list.ok) {
      set_status(StatusKind::Error, "Cloud backup response invalid.");
      return false;
    }
    for (const DriveFile &file : list.files) {
      if (file.name.size() < 4 || file.name.compare(file.name.size() - 4, 4, ".zip") != 0) {
        continue;
      }
      const auto folder = folder_id_to_name.find(file.parent_id);
      if (folder != folder_id_to_name.end()) {
        drive_index_[folder->second].push_back({file.name, file.id, file.size_bytes});
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
  rebuild_backup_rows();
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

void App::rebuild_backup_rows() {
  backup_rows_ = build_backup_rows(remote_backup_names(), local_backups_);
}

std::size_t App::backup_count() const {
  // Snapshot rows only; the "New Backup" sentinel at index 0 is not a backup.
  return backup_rows_.empty() ? 0 : backup_rows_.size() - 1;
}

const BackupRow *App::selected_backup_row() const {
  if (selected_backup_ == 0 || selected_backup_ >= backup_rows_.size()) {
    return nullptr;
  }
  return &backup_rows_[selected_backup_];
}

std::string App::selected_backup_name() const {
  const BackupRow *row = selected_backup_row();
  return row ? row->primary_name() : std::string();
}

// Re-locate a snapshot after the rows were rebuilt (new sibling, rename, upload); matching by
// identity keeps the focus on the same snapshot whichever side its name came from.
void App::focus_backup_row_by_identity(const std::string &backup_name) {
  const std::string identity = backup_identity(backup_name);
  for (std::size_t i = 1; i < backup_rows_.size(); ++i) {
    if (backup_identity(backup_rows_[i].primary_name()) == identity) {
      selected_backup_ = i;
      return;
    }
  }
}

std::string App::remote_file_id_for(const std::string &remote_name) const {
  for (const RemoteBackup &backup : remote_backups_) {
    if (backup.name == remote_name) {
      return backup.file_id;
    }
  }
  return {};
}

long long App::remote_size_for(const std::string &remote_name) const {
  for (const RemoteBackup &backup : remote_backups_) {
    if (backup.name == remote_name) {
      return backup.size_bytes;
    }
  }
  return 0;
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
  // Identity (timestamp prefix) instead of full-name equality: a label rename that reached only
  // one side must not look like a new backup, or the batch and the manual upload would stack a
  // duplicate under the other name.
  const std::string identity = backup_identity(backup_name);
  for (const RemoteBackup &remote : indexed->second) {
    if (backup_identity(remote.name) == identity) {
      return true;
    }
  }
  return false;
}

// Select moves the focused snapshot across: a card-only row uploads, a Drive-only row downloads
// a card copy without touching the live save (unlike restore, which downloads as a side effect).
bool App::download_remote_backup_to_card(const SaveRecord &save, const BackupRow &row,
                                         std::string *error) {
  if (!row.has_remote() || row.has_local()) {
    if (error) {
      *error = "This backup is already on the card.";
    }
    return false;
  }
  if (!ensure_google_access_token()) {
    return false;
  }
  const std::string file_id = remote_file_id_for(row.remote_name);
  if (file_id.empty()) {
    if (error) {
      *error = "Cloud copy not found; refresh and retry.";
    }
    return false;
  }
  const std::string archive_path =
      local_backup_archive_path(kBackupRoot, save.id, row.remote_name);
  const std::string temporary_path = archive_path + ".download";
  if (!ensure_parent_directory(archive_path)) {
    if (error) {
      *error = "Could not create local backup folder.";
    }
    return false;
  }

  // Download beside the final file, then publish only the complete ZIP. A failed stream cannot
  // appear in the backup list and an out-of-date row can never overwrite a card copy.
  std::remove(temporary_path.c_str());
  const std::string busy_label =
      ui_.compose_modal_label("Downloading ", display_backup_name(row.remote_name), "");
  BusyLabelScope busy(busy_label.c_str());
  const std::string download_url =
      std::string(kDriveFilesEndpoint) + "/" + form_url_encode(file_id) + "?alt=media";
  const HttpResponse download = drive_request([&](const std::string &token) {
    return HttpClient().download_file(download_url, temporary_path, token);
  });
  if (!download.ok) {
    std::remove(temporary_path.c_str());
    if (error) {
      *error = "Cloud download failed.";
    }
    return false;
  }
  std::string publish_error;
  if (!publish_backup_download(temporary_path, archive_path, &publish_error)) {
    if (error) {
      *error = publish_error == "backup already exists"
                   ? "This backup is already on the card."
                   : "Could not save the downloaded backup.";
    }
    return false;
  }
  refresh_local_backups();
  focus_backup_row_by_identity(row.remote_name);
  if (error) {
    error->clear();
  }
  return true;
}

void App::handle_transfer_button() {
  cancel_sync_all_confirmation();
  cancel_duplicate_backup_confirmation();
  delete_scope_prompt_pending_ = false;
  const SaveRecord *selected = selected_save_record();
  if (!selected) {
    set_status(StatusKind::Info, "No save selected.");
    return;
  }
  const BackupRow *selected_row = selected_backup_row();
  if (!selected_row) {
    set_status(StatusKind::Info, "Select a backup to upload or download.");
    return;
  }
  const BackupRow row = *selected_row;

  if (!row.has_local()) {
    std::string error;
    if (!download_remote_backup_to_card(*selected, row, &error)) {
      if (!error.empty()) {
        set_status(StatusKind::Error, std::move(error));
      }
      return;
    }
    set_status(StatusKind::Success,
               ui_.compose_status_with_name("Downloaded ", display_backup_name(row.remote_name),
                                            "."));
    return;
  }

  const std::string backup_name = row.local_name;

  // A synced row advertises no Select action in the footer, so a tap is a silent no-op there -
  // an "already on Drive" toast would nag about a button the UI never offered. (The hold gesture
  // still runs the batch from any row.)
  if (row.has_remote()) {
    return;
  }
  // A snapshot never changes after creation, so a Drive file with the same timestamp identity is
  // the same backup (even under a stale pre-rename name); skip the upload instead of stacking
  // duplicates (Drive allows same-name siblings). Unlike the synced-row case the footer did
  // offer Upload here, so the refusal explains itself.
  if (remote_backup_exists(selected->id, backup_name)) {
    set_status(StatusKind::Info, "This backup is already in the Cloud.");
    return;
  }

  if (!ensure_google_access_token()) {
    return;
  }

  const std::string busy_label =
      ui_.compose_modal_label("Uploading ", display_backup_name(backup_name), "");
  BusyLabelScope busy(busy_label.c_str());
  const BackupUploadResult uploaded = upload_local_backup(*selected, backup_name);
  if (!uploaded.ok) {
    return;
  }
  refresh_remote_backups_view();
  // The rebuild may have shifted the rows; keep the selection on the file this action was about.
  focus_backup_row_by_identity(backup_name);
  if (uploaded.metadata_warning) {
    set_status(StatusKind::Info, "Backup uploaded, but slot details were not synced.");
  } else {
    set_status(StatusKind::Success,
               ui_.compose_status_with_name("Uploaded ", display_backup_name(backup_name),
                                            " to the Cloud."));
  }
}

SaveMetadataJsonResult App::ensure_local_backup_metadata(const SaveRecord &save,
                                                         const std::string &backup_name) {
  const std::string identity = backup_identity(backup_name);
  const std::string metadata_path =
      local_backup_metadata_path(kBackupRoot, save.id, backup_name);
  SaveMetadataJsonResult metadata = read_save_metadata_json(metadata_path);
  const bool usable_cached_metadata = save_metadata_is_usable(metadata, identity);
  if (usable_cached_metadata && metadata.metadata.source == SaveTimeSource::VitaSlot) {
    upgrade_legacy_metadata_file(metadata_path, identity, &metadata);
    return metadata;
  }

  // Old Save Keeper versions wrote ZIPs only. Read the one small slot file directly from the
  // archive; never restore or unpack the save merely to show its details.
  const std::string archive_path =
      local_backup_archive_path(kBackupRoot, save.id, backup_name);
  const ArchiveReadResult embedded =
      read_stored_backup_entry(archive_path, "sce_sys/sdslot.dat", kMaxSdslotFileSize);
  SaveMetadata recovered;
  if (embedded.ok) {
    recovered = parse_sdslot_data(embedded.data);
  }

  // PSP/homebrew saves normally have no Vita slot file. Their cached filesystem time is already
  // the best available metadata, so do not repeatedly unpack the same archive on every visit.
  if (embedded.entry_missing() && usable_cached_metadata) {
    upgrade_legacy_metadata_file(metadata_path, identity, &metadata);
    return metadata;
  }

  // Retail backups contain raw PFS-encrypted files, so their embedded sdslot.dat looks like
  // random bytes until the whole save directory is mounted. Extract into an isolated work path;
  // never mount over the live save merely to inspect an old backup.
  if (recovered.slots.empty()) {
    // AppMgr accepts savedata mounts only from a savedata device root. Use the alternate ur0 root
    // so inspection can never collide with the game's live ux0 save. The fixed title-ID-shaped
    // folder belongs only to Save Keeper and is removed by BackupInspectionDirectory.
    const std::string work_root = "ur0:user/00/savedata";
    if (ensure_directory_path(work_root)) {
      BackupInspectionDirectory inspection(work_root + "/SVKMTMP01");
      const RestoreResult extracted =
          extract_backup_archive_for_inspection(archive_path, inspection.path());
      if (extracted.ok) {
        SaveMetadata mounted = resolve_live_save_metadata(
            inspection.path(), {}, save.platform != SavePlatform::Psp, mount_bridge_ready_);
        if ((mounted.source == SaveTimeSource::VitaSlot && !mounted.slots.empty()) ||
            (mounted.source == SaveTimeSource::Filesystem &&
             !extracted.file_timestamps_uniform)) {
          recovered = std::move(mounted);
        }
      }
    }
  }

  if (!save_metadata_has_observed_time(recovered)) {
    // A healthy filesystem-time companion is still useful for games that do not use Vita slots.
    // Preserve it after the one best-effort archive inspection rather than replacing it with an
    // invented backup time.
    if (usable_cached_metadata) {
      return metadata;
    }
    // Do not cache the failure. Inspection can fail for transient/environmental reasons - a mount
    // that could not be satisfied this session, or not enough ur0: space to extract - and a cached
    // "nothing here" marker would permanently hide slot details that are actually recoverable. This
    // path is user-initiated (opening details), not per-frame, so re-inspecting next time is cheap.
    return {false, {}, {}, "slot details unavailable"};
  }
  std::string write_error;
  if (!write_save_metadata_json_atomic(metadata_path, identity, recovered, &write_error)) {
    return {false, {}, {}, write_error};
  }
  return {true, identity, std::move(recovered), {}};
}

DriveFile App::find_remote_sidecar(const std::string &folder_id,
                                   const std::string &archive_file_id,
                                   const std::string &archive_name) {
  const auto find_first = [&](const std::string &query) {
    const std::string url = std::string(kDriveFilesEndpoint) + "?" + query;
    const HttpResponse response = drive_request([&](const std::string &token) {
      return HttpClient().get_json(url, token);
    });
    if (!response.ok) {
      return DriveFile{};
    }
    const DriveFileList files = parse_drive_file_list(response.body);
    return files.ok && !files.files.empty() ? files.files[0] : DriveFile{};
  };

  // File IDs survive user renames on Drive, making the private property the reliable link.
  DriveFile sidecar = find_first(
      build_drive_find_sidecar_by_archive_query(folder_id, archive_file_id));
  if (sidecar.id.empty()) {
    // Name lookup supports companions created before the property was introduced.
    sidecar = find_first(
        build_drive_find_child_by_name_query(folder_id, backup_metadata_name(archive_name)));
  }
  return sidecar;
}

SaveMetadataJsonResult App::download_remote_backup_metadata(
    const SaveRecord &save, const std::string &archive_name,
    const std::string &archive_file_id) {
  const std::string metadata_path =
      local_backup_metadata_path(kBackupRoot, save.id, archive_name);
  const std::string expected_identity = backup_identity(archive_name);
  const SaveMetadataJsonResult cached = read_save_metadata_json(metadata_path);
  if (save_metadata_is_usable(cached, expected_identity)) {
    SaveMetadataJsonResult upgraded = cached;
    upgrade_legacy_metadata_file(metadata_path, expected_identity, &upgraded);
    return upgraded;
  }

  const std::string folder_name = resolved_drive_folder_name(save.id);
  const auto folder = drive_folder_ids_.find(folder_name);
  if (folder == drive_folder_ids_.end()) {
    return {false, {}, {}, "Cloud save folder unavailable"};
  }
  const DriveFile sidecar =
      find_remote_sidecar(folder->second, archive_file_id, archive_name);
  if (sidecar.id.empty()) {
    return {false, {}, {}, kNoRemoteSidecarError};
  }
  if (!ensure_parent_directory(metadata_path)) {
    return {false, {}, {}, "could not create metadata folder"};
  }

  // Download beside the cache and validate first. A broken response must not replace a healthy
  // companion left by an earlier run.
  const std::string temporary_path = metadata_path + ".download";
  const std::string url = std::string(kDriveFilesEndpoint) + "/" +
                          form_url_encode(sidecar.id) + "?alt=media";
  const HttpResponse downloaded = drive_request([&](const std::string &token) {
    return HttpClient().download_file(url, temporary_path, token);
  });
  if (!downloaded.ok) {
    std::remove(temporary_path.c_str());
    return {false, {}, {}, "slot details download failed"};
  }
  SaveMetadataJsonResult metadata = read_save_metadata_json(temporary_path);
  if (!save_metadata_is_usable(metadata, expected_identity)) {
    std::remove(temporary_path.c_str());
    metadata.ok = false;
    metadata.error = "slot details do not match this backup";
    return metadata;
  }
  if (!upgrade_legacy_metadata_file(temporary_path, expected_identity, &metadata)) {
    std::remove(temporary_path.c_str());
    return metadata;
  }
  if (std::rename(temporary_path.c_str(), metadata_path.c_str()) != 0) {
    // The details were downloaded and validated; only caching them for next time failed. Return
    // the in-memory metadata so the screen can still show it instead of "unavailable".
    std::remove(temporary_path.c_str());
    metadata.error.clear();
  }
  return metadata;
}

void App::repair_remote_backup_metadata(const SaveRecord &save, const BackupRow &row,
                                        bool replace_unusable) {
  if (!row.has_remote() || !google_connected_ || !network_connected_) {
    return;
  }
  const std::string metadata_path =
      local_backup_metadata_path(kBackupRoot, save.id, row.primary_name());
  SaveMetadataJsonResult local_metadata = read_save_metadata_json(metadata_path);
  if (!save_metadata_is_usable(local_metadata, backup_identity(row.primary_name()))) {
    return;
  }
  if (!upgrade_legacy_metadata_file(metadata_path, backup_identity(row.primary_name()),
                                    &local_metadata)) {
    return;
  }
  const std::string archive_file_id = remote_file_id_for(row.remote_name);
  const std::string folder_name = resolved_drive_folder_name(save.id);
  const auto folder = drive_folder_ids_.find(folder_name);
  if (archive_file_id.empty() || folder == drive_folder_ids_.end() ||
      !ensure_google_access_token()) {
    return;
  }

  // Do not create duplicate companions if Drive already has either the stable property link or
  // an older canonical-name companion. Repair is opportunistic; the details screen never waits
  // for it and never reports it as a ZIP failure.
  const DriveFile existing =
      find_remote_sidecar(folder->second, archive_file_id, row.remote_name);
  if (!existing.id.empty()) {
    if (!replace_unusable) {
      return;
    }
    // Update in place: Drive keeps the old JSON if this request fails, and the stable file ID
    // prevents another device from observing a delete/create gap or duplicate companions.
    drive_request([&](const std::string &token) {
      return HttpClient().patch_multipart_file(
          build_drive_multipart_update_url(existing.id),
          build_drive_sidecar_update_metadata_json(
              backup_metadata_name(row.remote_name), archive_file_id),
          metadata_path, "application/json", token);
    });
    return;
  }
  drive_request([&](const std::string &token) {
    return HttpClient().post_multipart_file(
        kDriveUploadEndpoint,
        build_drive_sidecar_upload_metadata_json(
            backup_metadata_name(row.remote_name), folder->second, archive_file_id),
        metadata_path, "application/json", token);
  });
}

void App::download_and_inspect_selected_backup() {
  const SaveRecord *selected = selected_save_record();
  const BackupRow *selected_row = selected_backup_row();
  if (!selected || !selected_row || !slot_details_.download_to_inspect_available) {
    return;
  }
  const SaveRecord save = *selected;
  const BackupRow remote_row = *selected_row;
  std::string error;
  if (!download_remote_backup_to_card(save, remote_row, &error)) {
    slot_details_.warning_message =
        error.empty() ? "The backup could not be downloaded." : std::move(error);
    return;
  }

  // The ZIP is now a normal card backup regardless of whether optional slot information exists.
  // Recover locally, replace an unusable Drive companion when possible, then redraw this screen.
  const SaveMetadataJsonResult metadata =
      ensure_local_backup_metadata(save, remote_row.remote_name);
  if (save_metadata_is_usable(metadata, backup_identity(remote_row.remote_name))) {
    repair_remote_backup_metadata(save, remote_row, true);
  }
  open_save_details();
}

void App::request_save_details() {
  const BackupRow *row = selected_backup_row();
  const SaveRecord *save = selected_save_record();
  // The live-save ("New Backup") row shows a time still being read through a mount. Opening now
  // would race that background read and the press could be swallowed by it, so defer the open until
  // the time lands (the main loop tick fires it, and any other input cancels it). Backup rows carry
  // their own metadata, so they open immediately.
  if (row && row->new_backup && save && save->save_time_requires_mount) {
    details_open_pending_ = true;
    return;
  }
  open_save_details();
}

void App::open_save_details() {
  const SaveRecord *selected = selected_save_record();
  const BackupRow *selected_row = selected_backup_row();
  if (!selected) {
    return;
  }
  const SaveRecord save = *selected;
  ui_.draw_busy("Loading save details", 0, -1);

  slot_details_ = {};
  slot_details_.game_title = save.display_name;
  if (!selected_row) {
    // "New Backup" represents the live save. Resolve its details directly without creating an
    // archive or JSON companion; this is a read-only preview of what the next backup would use.
    slot_details_.snapshot_name = "Current Save";
    slot_details_.metadata = resolve_live_save_metadata(
        save.path, {}, save.platform != SavePlatform::Psp, mount_bridge_ready_);
    if (!save_metadata_has_observed_time(slot_details_.metadata)) {
      slot_details_.metadata = {};
      slot_details_.unavailable_message = "No save details available";
      slot_details_.warning_message =
          "No readable save slot information or save-file timestamp was found.";
    } else if (slot_details_.metadata.slots.empty()) {
      slot_details_.unavailable_message = "No save slot details available";
      slot_details_.warning_message =
          save.platform == SavePlatform::Psp
              ? "PSP saves do not use save slot metadata. The save time comes from the newest "
                "file in this save."
              : "This save has no readable save slot metadata. The save time comes from its "
                "newest file.";
    }
    // On-demand: the live save's on-disk footprint. No ZIP exists yet, so leave archive_bytes unset.
    bool save_size_ok = false;
    const std::uint64_t save_bytes = compute_folder_size(save.path, &save_size_ok);
    if (save_size_ok) {
      slot_details_.save_bytes = save_bytes;
      slot_details_.save_bytes_known = true;
    }
    slot_details_.open = true;
    return;
  }

  const BackupRow row = *selected_row;
  slot_details_.snapshot_name = row.primary_name();

  // A snapshot shows one size: its ZIP file. The archive stores entries uncompressed, so the
  // save content inside differs from the file size only by ZIP header overhead - showing both
  // was two nearly identical numbers. Independent of slot-metadata parsing below, so the size
  // shows even when the slot table cannot be read.
  if (row.has_local()) {
    bool zip_ok = false;
    const std::uint64_t zip_bytes = archive_file_size(
        local_backup_archive_path(kBackupRoot, save.id, row.local_name), &zip_ok);
    if (zip_ok) {
      slot_details_.archive_bytes = zip_bytes;
      slot_details_.archive_bytes_known = true;
    }
  }

  // A Cloud-only snapshot has no local file to measure, but the Drive listing already carried the
  // ZIP's size, so at least that shows without downloading anything.
  if (!slot_details_.archive_bytes_known && row.has_remote()) {
    const long long remote_bytes = remote_size_for(row.remote_name);
    if (remote_bytes > 0) {
      slot_details_.archive_bytes = static_cast<std::uint64_t>(remote_bytes);
      slot_details_.archive_bytes_known = true;
    }
  }

  SaveMetadataJsonResult metadata;

  if (row.has_local()) {
    const std::string metadata_path =
        local_backup_metadata_path(kBackupRoot, save.id, row.local_name);
    const bool already_cached = save_metadata_is_usable(
        read_save_metadata_json(metadata_path), backup_identity(row.local_name));
    metadata = ensure_local_backup_metadata(save, row.local_name);
    if (metadata.ok && !already_cached && row.has_remote()) {
      repair_remote_backup_metadata(save, row);
    }
  }

  // A missing local companion may still exist on Drive. Fetch only this one small JSON file when
  // the user asks for details; the startup index remains ZIP-only and fast.
  if (!metadata.ok && row.has_remote() && network_connected_ && ensure_google_access_token()) {
    metadata = download_remote_backup_metadata(save, row.remote_name,
                                                remote_file_id_for(row.remote_name));
  }

  const bool usable_metadata =
      save_metadata_is_usable(metadata, backup_identity(row.primary_name()));
  slot_details_.download_to_inspect_available =
      backup_details_download_available(row, usable_metadata);

  if (usable_metadata) {
    slot_details_.metadata = std::move(metadata.metadata);
  } else {
    // Old Save Keeper ZIPs used backup creation time in their names and entry headers, so neither
    // is evidence of when the game was saved. Leave the time unknown instead of presenting it as
    // an estimate.
    slot_details_.unavailable_message = "No slot details available";
    if (!row.has_local()) {
      // Say why the Cloud lookup came up empty - being offline, a backup whose details were never
      // uploaded, and a failed fetch read very differently to the user. All three end with the
      // same escape hatch: download the ZIP and inspect it locally.
      if (!network_connected_ || !google_connected_) {
        slot_details_.warning_message =
            "Google Drive is not connected. Download the backup to check for save slot "
            "information.";
      } else if (metadata.error == kNoRemoteSidecarError) {
        slot_details_.warning_message =
            "This backup has no details file in the Cloud. Download it to check for save slot "
            "information.";
      } else {
        slot_details_.warning_message =
            "Download the backup to check for save slot information.";
      }
    } else {
      slot_details_.warning_message =
          "No readable save slot information was found in this backup.";
    }
  }
  slot_details_.open = true;
}

// Sends one local archive to its save folder on Drive and slots it into the index. Error status
// is set here so both the single upload and the batch report the same failures.
BackupUploadResult App::upload_local_backup(const SaveRecord &save,
                                            const std::string &backup_name) {
  BackupUploadResult result;
  const std::string save_key = drive_folder_name_for(save.id);
  const std::string desired_name = drive_save_folder_name(save_key, save.display_name);

  if (drive_root_folder_id_.empty()) {
    drive_root_folder_id_ = find_or_create_drive_folder(kGoogleDriveRootFolderName, "root");
    if (drive_root_folder_id_.empty()) {
      return result;
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
        return result;
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
        archive_path, "application/zip", token);
  });
  if (!upload_response.ok) {
    set_status(StatusKind::Error, "Cloud upload failed.");
    return result;
  }

  // The upload response carries the new file's id; slotting it into the index directly keeps the
  // Drive list current without another full sync.
  const DriveFileList uploaded = parse_drive_file_list(upload_response.body);
  std::string archive_file_id;
  if (uploaded.ok && !uploaded.files.empty()) {
    archive_file_id = uploaded.files[0].id;
    std::vector<RemoteBackup> &list = drive_index_[folder_name];
    list.push_back({uploaded.files[0].name, uploaded.files[0].id});
    std::sort(list.begin(), list.end(),
              [](const RemoteBackup &a, const RemoteBackup &b) { return a.name > b.name; });
  } else {
    sync_drive_index();
  }

  // From this point the actual backup is safe on Drive. Any companion problem is only a warning.
  result.ok = true;
  const SaveMetadataJsonResult metadata =
      ensure_local_backup_metadata(save, backup_name);
  if (!metadata.ok || archive_file_id.empty()) {
    result.metadata_warning = true;
    return result;
  }
  const std::string metadata_path =
      local_backup_metadata_path(kBackupRoot, save.id, backup_name);
  const HttpResponse sidecar_upload = drive_request([&](const std::string &token) {
    return HttpClient().post_multipart_file(
        kDriveUploadEndpoint,
        build_drive_sidecar_upload_metadata_json(backup_metadata_name(backup_name), folder_id,
                                                 archive_file_id),
        metadata_path, "application/json", token);
  });
  result.metadata_warning = !sidecar_upload.ok;
  return result;
}

// PATCHes the Drive file's name and updates the cached index, mirroring the folder-rename block
// in upload_local_backup. Local rename happens first at the call site, so a failure here leaves a
// stale Drive name that identity matching tolerates and the next label edit heals (PATCH is by
// file id, not by name).
bool App::rename_remote_backup(const SaveRecord &save, const std::string &remote_name,
                               const std::string &new_name) {
  if (!ensure_google_access_token()) {
    return false;
  }
  const std::string file_id = remote_file_id_for(remote_name);
  if (file_id.empty()) {
    return false;
  }
  BusyLabelScope busy("Renaming Cloud backup", /*indeterminate=*/true);
  const std::string rename_url =
      std::string(kDriveFilesEndpoint) + "/" + form_url_encode(file_id) + "?fields=id%2Cname";
  const HttpResponse renamed = drive_request([&](const std::string &token) {
    return HttpClient().patch_json(rename_url, build_drive_rename_metadata_json(new_name), token);
  });
  if (!renamed.ok) {
    return false;
  }
  const std::string folder_name = resolved_drive_folder_name(save.id);
  const auto indexed = drive_index_.find(folder_name);
  if (indexed != drive_index_.end()) {
    for (RemoteBackup &backup : indexed->second) {
      if (backup.file_id == file_id) {
        backup.name = new_name;
        break;
      }
    }
    std::sort(indexed->second.begin(), indexed->second.end(),
              [](const RemoteBackup &a, const RemoteBackup &b) { return a.name > b.name; });
  }
  return true;
}

void App::begin_label_edit() {
  // Silent clears: opening the keyboard is itself the new context, a "canceled" status line
  // under the IME would only be noise.
  restore_confirmation_pending_ = false;
  delete_confirmation_pending_ = false;
  delete_scope_prompt_pending_ = false;
  duplicate_backup_confirmation_pending_ = false;
  sync_all_confirmation_pending_ = false;
  if (google_auth_pending_) {
    return;
  }
  const SaveRecord *selected = selected_save_record();
  if (!selected) {
    set_status(StatusKind::Info, "No save selected.");
    return;
  }
  const BackupRow *selected_row = selected_backup_row();
  if (!selected_row) {
    set_status(StatusKind::Info, "Select a backup to label.");
    return;
  }
  const BackupRow row = *selected_row;
  const std::string primary = row.primary_name();
  if (!has_backup_timestamp_prefix(primary)) {
    // A foreign zip without the timestamp identity cannot be renamed in place safely.
    set_status(StatusKind::Error, "This backup cannot be labeled.");
    return;
  }

  std::string entered;
  const TextInputResult input = ui_.prompt_text_input(
      "Backup label", backup_label(primary), kMaxBackupLabelLength, &entered);
  if (input == TextInputResult::Failed) {
    set_status(StatusKind::Error, "Could not open the keyboard.");
    return;
  }
  if (input == TextInputResult::Canceled) {
    return;
  }

  const std::string label = sanitize_backup_label(entered);
  const std::string new_name = backup_name_with_label(primary, label);
  const bool remote_stale = row.has_remote() && row.remote_name != new_name;
  if (new_name == primary && !remote_stale) {
    set_status(StatusKind::Info, "Label unchanged.");
    return;
  }
  if (backup_label_conflicts_with_auto(label)) {
    set_status(StatusKind::Error, "\"auto\" is reserved for automatic backups.");
    return;
  }

  if (row.has_local() && new_name != row.local_name) {
    const std::string old_path =
        local_backup_archive_path(kBackupRoot, selected->id, row.local_name);
    const std::string new_path = local_backup_archive_path(kBackupRoot, selected->id, new_name);
    if (std::rename(old_path.c_str(), new_path.c_str()) != 0) {
      // Leaving both sides untouched beats a half-renamed pair.
      set_status(StatusKind::Error, "Could not rename the card copy.");
      return;
    }
  }
  bool drive_ok = true;
  if (remote_stale) {
    drive_ok = rename_remote_backup(*selected, row.remote_name, new_name);
  }

  refresh_local_backups();
  refresh_remote_backups_view();
  focus_backup_row_by_identity(new_name);

  if (!drive_ok) {
    set_status(StatusKind::Error, row.has_local()
                                      ? "Renamed on the card, but the Cloud rename failed."
                                      : "Cloud rename failed.");
    return;
  }
  set_status(StatusKind::Success,
             label.empty()
                 ? std::string("Label removed.")
                 : ui_.compose_status_with_name("Labeled ", display_backup_name(new_name), "."));
}

void App::cancel_sync_all_confirmation() {
  if (sync_all_confirmation_pending_) {
    sync_all_confirmation_pending_ = false;
    set_status(StatusKind::Info, "Backup & upload canceled.");
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
    clear_status();
  }
}

void App::begin_sync_all() {
  restore_confirmation_pending_ = false;
  delete_confirmation_pending_ = false;
  delete_scope_prompt_pending_ = false;
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
  std::size_t metadata_warnings = 0;
  bool auth_lost = false;
  const bool drive_online = google_connected_ && HttpClient::network_reachable();
  batch_running_ = true;
  batch_cancel_requested_ = false;
  // The batch only uploads; a folder lookup/create response is the only download, and reporting
  // its bytes would flash the per-file percent to 100% just before the upload counts from 0.
  HttpClient::set_report_downloads(false);

  for (std::size_t i = 0; i < total; ++i) {
    // Cancel lands between games (polled here) or mid-upload (polled by the HTTP cancel check);
    // a zip in flight always completes so no partial archive is left behind.
    if (poll_batch_cancel()) {
      run.games_left = total - i;
      break;
    }

    const SaveRecord &save = saves_[targets[i]];
    ui_.set_batch_progress("Checking", save.display_name, i, total, enter_is_cross_);
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
      ui_.set_batch_progress("Backing up", save.display_name, i, total, enter_is_cross_);
      ui_.draw_busy("", 0, -1);
      const LocalSnapshotResult result = create_local_snapshot(save, "", false);
      if (!result.ok) {
        // The planned upload was this archive; there is nothing to send for this game.
        ++run.failed;
        continue;
      }
      if (!result.reused) {
        ++run.backed_up;
      }
      upload_name = result.archive_name;
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
    ui_.set_batch_progress("Uploading", save.display_name, i, total, enter_is_cross_);
    // The batch modal draws its own title from the action + game; this only needs to be non-empty
    // so the transfer-progress callback reports the per-file upload percent.
    const std::string upload_label = "Uploading " + save.display_name;
    BusyLabelScope busy(upload_label.c_str());
    const BackupUploadResult uploaded = upload_local_backup(save, upload_name);
    if (uploaded.ok) {
      ++run.uploaded;
      if (uploaded.metadata_warning) {
        ++metadata_warnings;
      }
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
  HttpClient::set_report_downloads(true);
  ui_.clear_batch_progress();

  refresh_local_backups();
  refresh_remote_backups_view();
  if (run.uploaded > 0 && sort_mode_ == SaveSortMode::LastBackup) {
    apply_sort_and_rebuild();
  }
  std::string summary = sync_run_summary(run);
  if (auth_lost) {
    summary += " Google session expired.";
  }
  if (metadata_warnings > 0) {
    summary += " Some slot details were not synced.";
  }
  set_status(run.failed > 0 ? StatusKind::Error : StatusKind::Success, std::move(summary));
}

int App::run() {
  if (!ui_.initialize()) {
    return -1;
  }

  // Slot timestamps are optional. If the mount bridge cannot load, all backup operations still
  // work and metadata falls back to save-file times as before. The result gates the kernel-bridge
  // syscall so a failed load degrades to the AppMgr mount instead of calling an unloaded module.
  mount_bridge_ready_ = initialize_save_data_mount_bridge();

  // Scanning storage and reading the system app database (titles, icons) blocks for a moment on a
  // full library; draw a frame first so the screen is not blank while it runs. Start at a
  // determinate 0% (total 1 is a placeholder until the scan lists the saves and reports the real
  // count) so the bar opens at 0% rather than flashing an indeterminate sweep before it fills.
  ui_.draw_busy("Loading saves", 0, 1);

  // Scan once at startup for the foundation build. Later actions that create, restore, or delete a
  // save will refresh this list explicitly so the UI does not rescan storage every frame.
  saves_ = scan_save_roots(default_save_roots(),
                           [this](std::size_t done, std::size_t total) {
                             ui_.draw_busy("Loading saves", static_cast<long long>(done),
                                           static_cast<long long>(total));
                           });
  // The app-database query (titles, icons) has no known row count, so it stays a pulse after the
  // determinate scan above; repaint a frame every few rows so it does not look frozen.
  const AppDbMetadataResult metadata_result =
      apply_app_db_metadata(&saves_, [this] { ui_.draw_busy("Loading saves", 0, -1); });
  if (!metadata_result.ok && !metadata_result.error.empty()) {
    set_status(StatusKind::Info, "Using save-folder metadata: " + metadata_result.error);
  }
  // Cached mount-resolved times: every save whose folder fingerprint is unchanged since its time
  // was last read skips the mount entirely, so the "Reading save times" pass below only touches
  // games that were actually played (or restored) since the previous launch.
  save_time_cache_ = read_save_time_cache(kSaveTimeCachePath);
  apply_save_time_cache();
  load_settings();
  if (save_sort_requires_all_times(sort_mode_) && !resolve_all_save_times()) {
    // Canceled the startup read (the saved sort was Last Saved); fall back to name.
    sort_mode_ = SaveSortMode::Name;
    save_settings();
  }
  apply_save_sort(&saves_, sort_mode_, {});
  // Open on the first tab that actually has saves so the app never starts on an empty grid.
  for (int i = 0; i < kSaveCategoryCount; ++i) {
    if (category_count(static_cast<SaveCategory>(i)) > 0) {
      category_ = static_cast<SaveCategory>(i);
      break;
    }
  }
  rebuild_visible_saves();
  schedule_selected_save_time_resolve();
  refresh_local_backups();
  load_google_token_cache();

  // Bring the network stack up once for the whole run. Doing this per request was fragile: a
  // second initialization of an already-running stack fails and every request after that failed.
  // It blocks for a beat, so keep a frame on screen while it happens.
  ui_.draw_busy("Starting network", 0, -1);
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

  // The IME dialog (backup labels) renders through the common dialog layer; it follows the
  // console's language and enter-button settings only if they are handed over once at startup.
  SceCommonDialogConfigParam common_dialog_config;
  sceCommonDialogConfigParamInit(&common_dialog_config);
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG,
                              reinterpret_cast<int *>(&common_dialog_config.language));
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON,
                              reinterpret_cast<int *>(&common_dialog_config.enterButtonAssign));
  sceCommonDialogSetConfigParam(&common_dialog_config);

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  // With a stored sign-in, load the Drive index right away so every game shows its Drive backups
  // without a manual refresh; the progress overlay covers the wait.
  if (google_connected_) {
    if (sync_drive_index()) {
      refresh_remote_backups_view();
      if (sort_mode_ == SaveSortMode::LastBackup) {
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
  int square_hold_frames = 0;
  bool square_hold_consumed = false;
  int triangle_hold_frames = 0;
  bool triangle_hold_consumed = false;
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

    // Details is deliberately isolated from the normal action map. Only slot navigation,
    // description scrolling, and Back are accepted, so an accidental press cannot restore,
    // delete, upload, or create a backup behind this screen.
    if (slot_details_.open) {
      if ((pressed & cancel_button) != 0) {
        slot_details_.open = false;
        previous_buttons = buttons;
        sceKernelDelayThread(kFrameDelayUs);
        continue;
      }
      if ((pressed & backup_button) != 0 &&
          slot_details_.download_to_inspect_available) {
        download_and_inspect_selected_backup();
        previous_buttons = buttons;
        sceKernelDelayThread(kFrameDelayUs);
        continue;
      }
      if (!slot_details_.metadata.slots.empty()) {
        if ((pressed & SCE_CTRL_UP) != 0) {
          slot_details_.selected_slot =
              slot_details_.selected_slot == 0
                  ? slot_details_.metadata.slots.size() - 1
                  : slot_details_.selected_slot - 1;
          slot_details_.details_scroll = 0;
        }
        if ((pressed & SCE_CTRL_DOWN) != 0) {
          slot_details_.selected_slot =
              (slot_details_.selected_slot + 1) % slot_details_.metadata.slots.size();
          slot_details_.details_scroll = 0;
        }
      }

      int details_scroll_direction = 0;
      if (pad.ry < kAnalogCenter - kAnalogDeadZone) {
        details_scroll_direction = -1;
      } else if (pad.ry > kAnalogCenter + kAnalogDeadZone) {
        details_scroll_direction = 1;
      }
      bool move_details_scroll = false;
      if (details_scroll_direction != rstick_direction_prev) {
        rstick_frames = 0;
        move_details_scroll = details_scroll_direction != 0;
      } else if (details_scroll_direction != 0) {
        ++rstick_frames;
        move_details_scroll =
            rstick_frames >= kRepeatInitialDelayFrames &&
            ((rstick_frames - kRepeatInitialDelayFrames) % kRepeatIntervalFrames) == 0;
      }
      rstick_direction_prev = details_scroll_direction;
      if (move_details_scroll) {
        slot_details_.details_scroll = std::min(
            ui_.details_max_scroll(slot_details_),
            std::max(0, slot_details_.details_scroll + details_scroll_direction));
      }

      UiState details_ui;
      details_ui.enter_is_cross = enter_is_cross_;
      details_ui.slot_details = &slot_details_;
      ui_.draw(details_ui);
      previous_buttons = buttons;
      sceKernelDelayThread(kFrameDelayUs);
      continue;
    }

    // Any input other than the Triangle press itself cancels a deferred details open, so a change
    // of mind (or moving to another save) never pops the screen open once the time resolves. The
    // right stick browses backups below and cancels it too, via move_selected_backup.
    if (details_open_pending_ && (pressed & ~static_cast<unsigned int>(SCE_CTRL_TRIANGLE)) != 0) {
      details_open_pending_ = false;
    }

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
    // Triangle mirrors the other tap/hold gestures: a tap opens save details, while a deliberate
    // hold performs the Google action. Delete scope and an active sign-in keep their immediate
    // Triangle actions because waiting for release would make those modal controls feel broken.
    if ((buttons & SCE_CTRL_TRIANGLE) != 0) {
      if ((pressed & SCE_CTRL_TRIANGLE) != 0 && delete_scope_prompt_pending_) {
        delete_scope_prompt_pending_ = false;
        perform_scoped_delete(false, true);
        triangle_hold_consumed = true;
      } else if ((pressed & SCE_CTRL_TRIANGLE) != 0 && google_auth_pending_) {
        handle_google_button();
        triangle_hold_consumed = true;
      } else if (!delete_scope_prompt_pending_) {
        ++triangle_hold_frames;
        if (resolve_tap_hold_action(triangle_hold_frames, false, triangle_hold_consumed,
                                    kSelectHoldTapFrames, kSelectHoldTriggerFrames) ==
            TapHoldAction::Hold) {
          triangle_hold_consumed = true;
          handle_google_button();
        }
      }
    } else {
      if (resolve_tap_hold_action(triangle_hold_frames, true, triangle_hold_consumed,
                                  kSelectHoldTapFrames, kSelectHoldTriggerFrames) ==
          TapHoldAction::Tap) {
        request_save_details();
      }
      triangle_hold_frames = 0;
      triangle_hold_consumed = false;
    }
    // Select: a tap (release within the tap window) transfers the focused backup (upload or
    // download, whichever side is missing); a one-second hold starts the tab-wide batch. Releasing
    // after the gauge appears but before the trigger is a back-out and does nothing.
    if ((buttons & SCE_CTRL_SELECT) != 0) {
      ++select_hold_frames;
      if (!select_hold_consumed && select_hold_frames >= kSelectHoldTriggerFrames) {
        select_hold_consumed = true;
        begin_sync_all();
      }
    } else {
      if (select_hold_frames > 0 && !select_hold_consumed &&
          select_hold_frames < kSelectHoldTapFrames) {
        handle_transfer_button();
      }
      select_hold_frames = 0;
      select_hold_consumed = false;
    }
    if ((pressed & SCE_CTRL_START) != 0) {
      handle_delete_button();
    }
    // Square mirrors the Select tap/hold split: a tap keeps its sort meaning, a one-second hold
    // opens the label editor for the focused backup. While the delete-scope prompt is open the
    // press means "card only" instead, and the spent press must not sort on release.
    if ((buttons & SCE_CTRL_SQUARE) != 0) {
      if ((pressed & SCE_CTRL_SQUARE) != 0 && delete_scope_prompt_pending_) {
        delete_scope_prompt_pending_ = false;
        perform_scoped_delete(true, false);
        square_hold_consumed = true;
      } else if (!delete_scope_prompt_pending_) {
        // Only grow the label-edit hold when no scope prompt is up; a Square held across the
        // Start press that opened the prompt must not silently graduate into a label edit.
        ++square_hold_frames;
        if (!square_hold_consumed && square_hold_frames >= kSelectHoldTriggerFrames) {
          square_hold_consumed = true;
          begin_label_edit();
        }
      }
    } else {
      // Same tap window as Select: a quick tap sorts; releasing after the gauge appears is a
      // back-out and must not change the sort.
      if (square_hold_frames > 0 && !square_hold_consumed &&
          square_hold_frames < kSelectHoldTapFrames) {
        cycle_sort_mode();
      }
      square_hold_frames = 0;
      square_hold_consumed = false;
    }

    // Once the selection has settled, resolve the focused save's mount-only time. Only the grid
    // path reaches here; the details mode returns earlier, so an open details screen never triggers
    // a background mount.
    if (pending_time_resolve_frames_ > 0) {
      --pending_time_resolve_frames_;
    } else if (pending_time_resolve_frames_ == 0) {
      pending_time_resolve_frames_ = -1;
      resolve_selected_save_time();
      if (details_open_pending_) {
        // A Triangle press was waiting on this resolve; honor it now that the time has landed.
        details_open_pending_ = false;
        open_save_details();
      }
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
    ui_state.backup_rows = &backup_rows_;
    ui_state.selected_backup = selected_backup_;
    ui_state.restore_confirmation_pending = restore_confirmation_pending_;
    ui_state.delete_confirmation_pending = delete_confirmation_pending_;
    ui_state.delete_scope_prompt_pending = delete_scope_prompt_pending_;
    ui_state.sync_all_confirmation_pending = sync_all_confirmation_pending_;
    ui_state.duplicate_backup_confirmation_pending = duplicate_backup_confirmation_pending_;
    ui_state.enter_is_cross = enter_is_cross_;
    ui_state.slot_details = &slot_details_;
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
    // Once Select or Square is held past the tap window, the status line explains what is about
    // to happen and a gauge fills from empty (at the tap window) to full (at the trigger); this
    // overlays the frame only, without touching the stored status, so a release restores it.
    const auto gauge_fraction = [](int frames) {
      return std::min(1.0f, std::max(0.0f, static_cast<float>(frames - kSelectHoldTapFrames) /
                                               static_cast<float>(kSelectHoldTriggerFrames -
                                                                  kSelectHoldTapFrames)));
    };
    if (!select_hold_consumed && select_hold_frames >= kSelectHoldTapFrames) {
      ui_state.hold_gauge_fraction = gauge_fraction(select_hold_frames);
      ui_state.status_message = sync_all_hold_message(save_category_label(category_));
      ui_state.status_kind = StatusKind::Info;
    } else if (!square_hold_consumed && square_hold_frames >= kSelectHoldTapFrames) {
      ui_state.hold_gauge_fraction = gauge_fraction(square_hold_frames);
      ui_state.status_message = selected_backup_row() != nullptr
                                    ? "Keep holding to edit this backup's label"
                                    : "Pick a backup first to give it a label";
      ui_state.status_kind = StatusKind::Info;
    } else if (!triangle_hold_consumed &&
               triangle_hold_frames >= kSelectHoldTapFrames) {
      ui_state.hold_gauge_fraction = gauge_fraction(triangle_hold_frames);
      ui_state.status_message = google_connected_
                                    ? "Keep holding to refresh Google Drive"
                                    : "Keep holding to connect Google Drive";
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
        if (sort_mode_ == SaveSortMode::LastBackup) {
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
