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
#include "core/TrackedFolders.hpp"
#include "vita/SaveAppDbMetadata.hpp"
#include "vita/mount/user/save_data_mount.h"
#include "vita/net/HttpClient.hpp"

#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/system_param.h>
#include <taihen.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <new>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
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
// Version 2 of save-titles.json is the merged index: times and titles in one file.
constexpr const char *kSaveIndexPath = "ux0:data/save-keeper/save-titles.json";
// The retired times half of the old two-file layout, removed at every boot.
constexpr const char *kLegacySaveTimesPath = "ux0:data/save-keeper/save-times.json";
// Backup-shaping settings (data folder overrides + the skip list) - the file other devices sync;
// device-local preferences stay in settings.txt.
constexpr const char *kBackupSettingsPath = "ux0:data/save-keeper/backup-settings.json";
// Where a conflict loser's local version is kept - written only when a Drive sync overwrites
// local changes (a losing remote version is recoverable from Drive's own revision history).
constexpr const char *kBackupSettingsConflictPath =
    "ux0:data/save-keeper/backup-settings.conflict.json";
// Known folder sets shipped inside the VPK (RetroArch first). Same schema as the user file; the
// user's backup-settings.json overrides it per entry id.
constexpr const char *kBaseDataFoldersPath = "app0:sce_sys/resources/savedata-paths.json";
// The file's name in the Drive "PSV Saves" root - the same name it carries on the card.
constexpr const char *kBackupSettingsDriveName = "backup-settings.json";
// The homebrew data convention root the folder browser is confined to. Distinct from kDataRoot
// above, which is this app's own config directory inside it.
constexpr const char *kUserDataRoot = "ux0:data";
// Bound the tracked-folders read: a truncated or absurdly large file must never be pulled whole
// into memory before parsing.
constexpr std::size_t kMaxTrackedFoldersJsonSize = 256 * 1024;
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
// Settle delay before the folder picker starts sizing the hovered row, so scrolling the list does
// not measure every folder the cursor merely passes.
constexpr int kBrowserSizeDelayFrames = 10;
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
// Frames of idle before the index is written (~1 s). Reads run on the mount worker and cost the
// main thread nothing, but the write still blocks a frame, so it waits for a pause deep enough
// that it is unlikely to be interrupted.
constexpr int kSaveIndexWriteIdleFrames = 60;
// Work that blocks a frame is bracketed by these two. The main loop peeks a single instantaneous
// pad sample per frame, so a button pressed and released inside the stall is never observed at
// all; the sample history carries it, and replaying it costs the user a few frames of delay
// instead of the press itself.
SceCtrlData arm_input_recovery() {
  SceCtrlData armed{};
  sceCtrlPeekBufferPositive(0, &armed, 1);
  return armed;
}

unsigned int buttons_pressed_since(const SceCtrlData &armed) {
  SceCtrlData pads[64] = {};
  const int count = sceCtrlPeekBufferPositive(0, pads, 64);
  unsigned int seen = 0;
  for (int i = 0; i < count; ++i) {
    if (pads[i].timeStamp > armed.timeStamp) {
      seen |= pads[i].buttons;
    }
  }
  return seen;
}

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

// The live directories of an entry's archive, each under its allocated zip path prefix, ready for
// the multi-source archive writer and the folder-signature check. An entry with no extra folders
// yields one flat source, so its archive stays byte-identical to one written before this feature.
std::vector<BackupSource> archive_sources(const SaveRecord &save) {
  const ArchiveLayout layout = archive_layout_for_record(save);
  std::vector<BackupSource> sources;
  sources.reserve(layout.sources.size());
  for (const TrackedPath &source : layout.sources) {
    sources.push_back({source.prefix, source.path});
  }
  return sources;
}

// Every live directory an entry covers - its savedata folder, then its extras - for
// resolve_tracked_metadata, which keeps the newest observed time across all of them.
std::vector<std::string> save_path_list(const SaveRecord &save) {
  std::vector<std::string> paths;
  paths.reserve(save.extra_paths.size() + 1);
  if (!save.path.empty()) {
    paths.push_back(save.path);
  }
  for (const TrackedPath &extra : save.extra_paths) {
    paths.push_back(extra.path);
  }
  return paths;
}

// Restore mapping derived from the live entry rather than the backup, used only as the fallback
// when a backup's sidecar carries no usable mapping of its own.
std::vector<RestoreTarget> entry_restore_targets(const SaveRecord &save) {
  const std::vector<TrackedPath> mapping =
      restore_targets_for_backup(save.path, archive_layout_for_record(save).extra_targets);
  std::vector<RestoreTarget> targets;
  targets.reserve(mapping.size());
  for (const TrackedPath &target : mapping) {
    targets.push_back({target.prefix, target.path, target.is_file});
  }
  return targets;
}

// Content signature over a save's live directories - a flat single folder when it has no extras,
// the per-prefix multi-source set once it does - so the duplicate/skip checks and the archive
// writer always measure the same files. Progress reaches the single-folder walk; the multi-source
// one is bounded by the picker's size caps and stays quick.
std::vector<ArchiveEntryInfo> compute_save_entries(
    const SaveRecord &save, bool *ok,
    const std::function<void(std::uint64_t, std::uint64_t)> &progress = {}) {
  if (!save.extra_paths.empty()) {
    return compute_sources_entries(archive_sources(save), ok);
  }
  return compute_folder_entries(save.path, ok, progress);
}

// Resolves and stores a record's save time from its live directories (newest observed file time).
// Used for entries whose time cannot come from a PFS mount: homebrew savedata and every extra
// data folder, none of which are encrypted.
void resolve_data_folder_time(SaveRecord *record) {
  const SaveMetadata metadata =
      resolve_tracked_metadata(save_path_list(*record), current_local_datetime());
  apply_mounted_save_time(record, metadata);
}

// The one function that mounts. Called from the mount worker thread only (or inline when the
// worker failed to start): every mount and unmount staying on a single thread is the structural
// guard against the AppMgr mount-slot leak that killed the earlier mixed-thread attempt. Main
// thread code goes through App::resolve_live_save_metadata, which hands the work over.
SaveMetadata resolve_live_metadata_on_mount_thread(const std::string &save_path,
                                                   const SaveDateTime &backup_clock,
                                                   bool allow_pfs_mount, bool bridge_available) {
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
    static constexpr int kSavedataMountIdCount =
        static_cast<int>(sizeof(kSavedataMountIds) / sizeof(kSavedataMountIds[0]));
    // Start from the id that worked last time. Every save on a given system mounts with the same
    // one, so scanning from the top again would spend failed syscalls on each read, and this path
    // now runs once per save the user scrolls onto rather than once in a while.
    static int preferred_mount_id = 0;
    for (int attempt = 0; attempt < kSavedataMountIdCount; ++attempt) {
      const int index = (preferred_mount_id + attempt) % kSavedataMountIdCount;
      args.id = kSavedataMountIds[index];
      mount_result = saveKeeperUserMountById(&args);
      if (mount_result >= 0) {
        preferred_mount_id = index;
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

// Single-operation transfer budget: while a scope is active, the progress hook maps every
// reported transfer onto one continuous bar over the operation's payload bytes. Small metadata
// requests hold the bar in place instead of sweeping, and a finished payload's bytes move into
// the base so a follow-up transfer (the companion) holds the bar full rather than restarting it.
// The batch never opens a scope; its corner percent keeps its own endpoint-suppression rules.
long long g_transfer_budget_base = 0;
long long g_transfer_budget_total = 0;

struct TransferBudgetScope {
  explicit TransferBudgetScope(long long total_bytes) {
    g_transfer_budget_base = 0;
    g_transfer_budget_total = total_bytes > 0 ? total_bytes : 0;
  }
  ~TransferBudgetScope() {
    g_transfer_budget_base = 0;
    g_transfer_budget_total = 0;
  }
  static void advance(long long bytes) { g_transfer_budget_base += bytes; }
  TransferBudgetScope(const TransferBudgetScope &) = delete;
  TransferBudgetScope &operator=(const TransferBudgetScope &) = delete;
};

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

bool path_is_directory(const std::string &path) {
  struct stat info {};
  return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Sorted names of the immediate child directories of root_path (dirs only, "." and ".." skipped).
// The core scanner has an equivalent (SaveScanner.cpp's list_direct_child_directories) but it is
// file-private, so the directory browser keeps its own. Names come straight from readdir and so can
// never contain a path separator; the browser builds child paths as current + "/" + name, which is
// what keeps it from ever escaping above its ux0:data root.
// Case-insensitive, like the game grid's title sort: byte order files ABM and VitaDB ahead of
// betterHomebrewBrowser, which reads as random on a mixed-case ux0:data. Byte order breaks ties
// so names differing only by case keep a stable order.
bool name_less_case_insensitive(const std::string &a, const std::string &b) {
  const std::size_t common = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < common; ++i) {
    const int left = std::tolower(static_cast<unsigned char>(a[i]));
    const int right = std::tolower(static_cast<unsigned char>(b[i]));
    if (left != right) {
      return left < right;
    }
  }
  if (a.size() != b.size()) {
    return a.size() < b.size();
  }
  return a < b;
}

std::vector<std::string> list_child_directories(const std::string &root_path) {
  std::vector<std::string> directories;
  DIR *dir = opendir(root_path.c_str());
  if (!dir) {
    return directories;
  }
  while (dirent *entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    if (path_is_directory(root_path + "/" + name)) {
      directories.push_back(name);
    }
  }
  closedir(dir);
  std::sort(directories.begin(), directories.end(),
            [](const std::string &a, const std::string &b) {
              return name_less_case_insensitive(a, b);
            });
  return directories;
}

// What the savedata-path picker lists: the immediate children of a directory, folders and plain
// files alike. Folders sort ahead of files (each group case-insensitively), the way every console
// file browser groups them; a file's size comes free with its dirent, so its row never needs the
// incremental walk.
struct BrowserChild {
  std::string name;
  bool is_file{};
  std::uint64_t size{};
};

std::vector<BrowserChild> list_browser_children(const std::string &root_path) {
  std::vector<BrowserChild> children;
  DIR *dir = opendir(root_path.c_str());
  if (!dir) {
    return children;
  }
  while (dirent *entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    if (SCE_S_ISDIR(entry->d_stat.st_mode)) {
      children.push_back({name, false, 0});
    } else if (SCE_S_ISREG(entry->d_stat.st_mode)) {
      children.push_back({name, true, static_cast<std::uint64_t>(entry->d_stat.st_size)});
    }
  }
  closedir(dir);
  std::sort(children.begin(), children.end(), [](const BrowserChild &a, const BrowserChild &b) {
    if (a.is_file != b.is_file) {
      return !a.is_file;
    }
    return name_less_case_insensitive(a.name, b.name);
  });
  return children;
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

std::string App::status_with_name(const std::string &prefix, const std::string &name,
                                  const std::string &suffix) const {
  // The details footer line runs to where its hints start; 560 is safely inside that for every
  // hint set this screen shows, and the renderer re-fits against the actual hints each frame.
  return ui_.compose_status_with_name(prefix, name, suffix, slot_details_.open ? 560 : 0);
}

LocalSnapshotResult App::create_local_snapshot(const SaveRecord &save,
                                               const std::string &suffix,
                                               const char *busy_label,
                                               bool force_new) {
  // The label's bar fills exactly once across the whole snapshot: the folder hash is the first
  // third and the archive's own two read passes are the rest, phase-offset below so the bar
  // continues instead of restarting.
  std::function<void(std::uint64_t, std::uint64_t)> check_progress;
  if (busy_label != nullptr) {
    check_progress = [this, busy_label](std::uint64_t done, std::uint64_t total) {
      ui_.draw_busy(busy_label, static_cast<long long>(done), static_cast<long long>(total * 3));
    };
  }
  LocalSnapshotResult snapshot;
  // Resolve once so the ZIP name and JSON describe the same moment, even if creating the archive
  // takes long enough for the wall clock to tick over. An entry carrying extra data folders takes
  // the newest time across all of its directories instead of going through a PFS mount.
  SaveMetadata metadata =
      save.extra_paths.empty()
          ? resolve_live_save_metadata(save.path, current_local_datetime(),
                                       save.platform != SavePlatform::Psp, mount_bridge_ready_)
          : resolve_tracked_metadata(save_path_list(save), current_local_datetime());
  // A backup that bundles extra folders records the prefix->directory mapping it was made from, so
  // restore reads its targets from the archive's own sidecar instead of a later, possibly edited,
  // config. The savedata destination is deliberately never recorded - the app supplies that from
  // the live entry - so a hand-edited sidecar can only ever redirect folders inside ux0:data. An
  // entry with no extras yields an empty list, keeping its sidecar byte-identical to before.
  metadata.tracked_targets = archive_layout_for_record(save).extra_targets;

  bool entries_ok = false;
  const std::vector<ArchiveEntryInfo> entries =
      compute_save_entries(save, &entries_ok, check_progress);
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
    // An entry with extra folders bundles several live directories under per-prefix paths; without
    // them it stays a single flat folder, which is what keeps its archives interchangeable with the
    // ones written before extra folders existed.
    if (save.extra_paths.empty()) {
      request.source_path = save.path;
    } else {
      request.sources = archive_sources(save);
    }
    request.backup_root = kBackupRoot;
    request.save_id = save.id;
    request.timestamp = timestamp;
    request.archive_name = plan.archive_name;
    if (busy_label != nullptr) {
      // The archive reports its two passes over (0..2x content); offsetting by the bytes the hash
      // above already covered turns the label's bar into one continuous 0..3x fill.
      std::uint64_t checked_bytes = 0;
      for (const ArchiveEntryInfo &entry : entries) {
        checked_bytes += entry.size;
      }
      request.progress = [this, busy_label, checked_bytes](std::uint64_t done,
                                                          std::uint64_t total) {
        ui_.draw_busy(busy_label, static_cast<long long>(checked_bytes + done),
                      static_cast<long long>(checked_bytes + total));
      };
    }
    const BackupResult backup = create_backup_archive(request);
    if (!backup.ok) {
      // create_backup_archive makes the folder before any failure it can realistically hit here.
      remove_local_backup_folder_if_empty(kBackupRoot, save.id);
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
    const AppSettings settings = parse_app_settings(text);
    sort_mode_ = settings.sort_mode;
    cleaned_empty_backup_folders_ = settings.cleaned_empty_backup_folders;
    backup_settings_synced_ = settings.backup_settings_synced;
  }
}

void App::save_settings() {
  AppSettings settings;
  settings.sort_mode = sort_mode_;
  // Every field has to be mirrored back here: this builds a fresh AppSettings, so anything left
  // out is erased from settings.txt the next time any other setting changes.
  settings.cleaned_empty_backup_folders = cleaned_empty_backup_folders_;
  settings.backup_settings_synced = backup_settings_synced_;
  write_text_file(kSettingsPath, serialize_app_settings(settings));
}

std::map<std::string, std::string> App::newest_backup_by_folder() const {
  // Keyed by the bare save key (what apply_save_sort derives from each save). Card and Drive
  // copies share the timestamped name identity, so one lexical max ranks a card-only backup the
  // same as a synced one - and works before the Drive index has synced at all.
  std::map<std::string, std::string> newest;
  for (const SaveRecord &save : saves_) {
    std::string best;
    for (const std::string &name : scan_local_backup_names(kBackupRoot, save.id)) {
      if (name > best) {
        best = name;
      }
    }
    const std::string folder_name = resolved_drive_folder_name(save.id);
    if (!folder_name.empty()) {
      const auto entry = drive_index_.find(folder_name);
      // Per-save lists are sorted newest first, so the first name is the latest sync point.
      if (entry != drive_index_.end() && !entry->second.empty() &&
          entry->second[0].name > best) {
        best = entry->second[0].name;
      }
    }
    if (!best.empty()) {
      newest[drive_folder_name_for(save.id)] = std::move(best);
    }
  }
  return newest;
}

void App::start_mount_worker() {
  mount_worker_wake_ = sceKernelCreateSema("save_keeper_mount_wake", 0, 0, 2, nullptr);
  if (mount_worker_wake_ < 0) {
    mount_worker_wake_ = -1;
    return;
  }
  mount_worker_thread_ = sceKernelCreateThread("save_keeper_mount", &App::mount_worker_entry,
                                               0x10000100, 0x20000, 0, 0, nullptr);
  if (mount_worker_thread_ < 0) {
    sceKernelDeleteSema(mount_worker_wake_);
    mount_worker_wake_ = -1;
    mount_worker_thread_ = -1;
    return;
  }
  App *self = this;
  sceKernelStartThread(mount_worker_thread_, sizeof(self), &self);
}

void App::stop_mount_worker() {
  if (mount_worker_thread_ < 0) {
    return;
  }
  mount_worker_stop_.store(true);
  sceKernelSignalSema(mount_worker_wake_, 1);
  sceKernelWaitThreadEnd(mount_worker_thread_, nullptr, nullptr);
  sceKernelDeleteThread(mount_worker_thread_);
  sceKernelDeleteSema(mount_worker_wake_);
  mount_worker_thread_ = -1;
  mount_worker_wake_ = -1;
}

int App::mount_worker_entry(unsigned int, void *argp) {
  App *app = *static_cast<App **>(argp);
  for (;;) {
    sceKernelWaitSema(app->mount_worker_wake_, 1, nullptr);
    if (app->mount_worker_stop_.load()) {
      return 0;
    }
    if (app->mount_work_state_.load() != 1) {
      continue;
    }
    MountWork &work = app->mount_work_;
    work.metadata = resolve_live_metadata_on_mount_thread(
        work.save_path, work.backup_clock, work.allow_pfs_mount, app->mount_bridge_ready_);
    if (work.want_fingerprint) {
      // After the unmount, so bookkeeping the mount touched is part of the stored state and the
      // next scan sees an unchanged folder. Blocking here is free - this is not the main thread.
      work.fingerprint = compute_save_fingerprint(work.save_path);
    }
    app->mount_work_state_.store(2);
  }
}

SaveMetadata App::resolve_live_save_metadata(const std::string &save_path,
                                             const SaveDateTime &backup_clock,
                                             bool allow_pfs_mount, bool bridge_available) {
  if (mount_worker_thread_ < 0) {
    // Worker never started: degrade to the old inline resolve. Still single-threaded - nothing
    // else can be mounting.
    return resolve_live_metadata_on_mount_thread(save_path, backup_clock, allow_pfs_mount,
                                                 bridge_available);
  }
  // A queued read may be in flight; land and apply it first so this request gets the slot.
  complete_async_read(true);
  mount_work_.save_path = save_path;
  mount_work_.backup_clock = backup_clock;
  mount_work_.allow_pfs_mount = allow_pfs_mount;
  mount_work_.want_fingerprint = false;
  mount_work_.async = false;
  mount_work_.discard = false;
  mount_work_state_.store(1);
  sceKernelSignalSema(mount_worker_wake_, 1);
  while (mount_work_state_.load() != 2) {
    sceKernelDelayThread(1000);
  }
  const SaveMetadata metadata = mount_work_.metadata;
  mount_work_state_.store(0);
  return metadata;
}

void App::submit_async_save_time_read(const SaveRecord &save) {
  mount_work_.save_path = save.path;
  mount_work_.backup_clock = {};
  mount_work_.allow_pfs_mount = true;
  mount_work_.want_fingerprint = true;
  mount_work_.async = true;
  mount_work_.discard = false;
  mount_work_.async_save_id = save.id;
  mount_work_state_.store(1);
  sceKernelSignalSema(mount_worker_wake_, 1);
}

void App::complete_async_read(bool wait) {
  if (!mount_work_.async) {
    return;
  }
  if (mount_work_state_.load() != 2) {
    if (!wait) {
      return;
    }
    while (mount_work_state_.load() != 2) {
      sceKernelDelayThread(1000);
    }
  }
  mount_work_.async = false;
  if (!mount_work_.discard) {
    for (SaveRecord &record : saves_) {
      if (record.id != mount_work_.async_save_id) {
        continue;
      }
      // A batch or a demand read may have resolved it while this was in flight; the stale result
      // must not overwrite the fresher one.
      if (!record.save_time_requires_mount) {
        break;
      }
      const bool resolved = apply_mounted_save_time(&record, mount_work_.metadata);
      // Same caching rule as the blocking path: skip when the bridge is down, that failure can
      // heal without the folder changing and those saves must retry next launch.
      if (mount_bridge_ready_) {
        record.fingerprint = mount_work_.fingerprint;
        if (record.fingerprint.ok) {
          SaveIndexEntry &entry = save_index_.entries[record.id];
          entry.fingerprint = record.fingerprint;
          entry.time_resolved = true;
          entry.has_time = resolved;
          entry.saved_at = resolved ? record.saved_at : SaveDateTime{};
          entry.from_app_db = record.title_from_app_db;
          entry.display_name = record.display_name;
          entry.title_id = record.title_id;
          entry.icon_path = record.icon_path;
          save_index_dirty_ = true;
        }
      }
      break;
    }
  }
  mount_work_.discard = false;
  mount_work_state_.store(0);
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
    // Fingerprint after the mount, so bookkeeping the mount itself touched is part of the
    // stored state and the next scan sees an unchanged folder.
    save->fingerprint = compute_save_fingerprint(save->path);
    if (save->fingerprint.ok) {
      // The record carries the title fields too (filled at scan time or by the app-db pass),
      // so the entry stays whole even when this creates it.
      SaveIndexEntry &entry = save_index_.entries[save->id];
      entry.fingerprint = save->fingerprint;
      entry.time_resolved = true;
      entry.has_time = resolved;
      // Zeroed when nothing was readable, matching the canonical form build_save_index writes.
      entry.saved_at = resolved ? save->saved_at : SaveDateTime{};
      entry.from_app_db = save->title_from_app_db;
      entry.display_name = save->display_name;
      entry.title_id = save->title_id;
      entry.icon_path = save->icon_path;
      save_index_dirty_ = true;
    }
  }
  return resolved;
}

void App::apply_cached_save_times() {
  for (SaveRecord &save : saves_) {
    // An entry with extra folders takes its time from resolve_tracked_metadata across all of its
    // directories. The cache is keyed on a fingerprint of the savedata folder alone, which would
    // not notice an extra folder changing, so those entries stay out of it entirely.
    if (!save.extra_paths.empty()) {
      continue;
    }
    if (!save.save_time_requires_mount || !save.fingerprint.ok) {
      continue;
    }
    const auto entry = save_index_.entries.find(save.id);
    if (entry == save_index_.entries.end() || !entry->second.time_resolved ||
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

void App::flush_save_index() {
  if (!save_index_dirty_) {
    return;
  }
  // Best effort: a failed write only means times are re-read (and titles re-derived) next boot.
  std::string error;
  if (write_save_index_atomic(kSaveIndexPath, save_index_, &error)) {
    save_index_dirty_ = false;
  }
}

void App::rebuild_save_index(long long app_db_mtime, long long app_db_size) {
  SaveIndex rebuilt = build_save_index(saves_, save_index_, app_db_mtime, app_db_size);
  // Serialized comparison doubles as the dirty check; skipping identical writes spares the
  // flash on the common boot where nothing changed.
  if (serialize_save_index(rebuilt) != serialize_save_index(save_index_)) {
    save_index_dirty_ = true;
  }
  save_index_ = std::move(rebuilt);
  flush_save_index();
}

namespace {

// Bounded whole-file read; false when missing, unreadable, or over the cap.
bool read_bounded_file(const char *path, std::string *text) {
  FILE *file = std::fopen(path, "rb");
  if (!file) {
    return false;
  }
  text->clear();
  char buffer[4096];
  std::size_t read = 0;
  bool oversized = false;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text->append(buffer, read);
    if (text->size() > kMaxTrackedFoldersJsonSize) {
      oversized = true;
      break;
    }
  }
  const bool read_error = std::ferror(file) != 0;
  std::fclose(file);
  return !oversized && !read_error;
}

} // namespace

void App::load_tracked_folders() {
  // The base file ships in the VPK, so a parse failure here is a build defect, not user data;
  // an empty base just means no built-in folder sets this run.
  std::string text;
  if (read_bounded_file(kBaseDataFoldersPath, &text)) {
    TrackedFoldersParseResult parsed = parse_tracked_folders_json(text);
    if (parsed.ok) {
      base_config_ = std::move(parsed.config);
    }
  }

  if (!read_bounded_file(kBackupSettingsPath, &text)) {
    struct stat info {};
    // Absent is the normal cold start; present but unreadable or implausibly large disables writes
    // so a later save never truncates a config we could not read whole.
    tracked_config_load_failed_ = stat(kBackupSettingsPath, &info) == 0;
    return;
  }
  const TrackedFoldersParseResult parsed = parse_tracked_folders_json(text);
  if (!parsed.ok) {
    // A truncated or corrupt config must survive untouched; persistence stays disabled this run.
    tracked_config_load_failed_ = true;
    return;
  }
  tracked_config_ = std::move(parsed.config);
}

bool App::save_data_folders_config() {
  std::string write_error;
  if (!write_tracked_folders_json_atomic(kBackupSettingsPath, tracked_config_, &write_error)) {
    set_status(StatusKind::Error, "Could not save backup-settings.json.");
    return false;
  }
  return true;
}

bool App::upload_backup_settings() {
  if (tracked_config_load_failed_ || !google_connected_) {
    return false;
  }
  if (drive_root_folder_id_.empty()) {
    drive_root_folder_id_ = find_or_create_drive_folder(kGoogleDriveRootFolderName, "root");
    if (drive_root_folder_id_.empty()) {
      return false;
    }
  }
  const std::string list_url =
      std::string(kDriveFilesEndpoint) + "?" +
      build_drive_find_child_by_name_query(drive_root_folder_id_, kBackupSettingsDriveName);
  const HttpResponse list_response = drive_request([&](const std::string &token) {
    return HttpClient().get_json(list_url, token);
  });
  if (!list_response.ok) {
    return false;
  }
  const DriveFileList files = parse_drive_file_list(list_response.body);
  if (!files.ok) {
    return false;
  }
  // Update in place when the file exists: Drive keeps the replaced content as a revision, which is
  // the recovery path for a lost conflict on the remote side.
  const HttpResponse upload = drive_request([&](const std::string &token) {
    if (!files.files.empty()) {
      return HttpClient().patch_multipart_file(
          build_drive_multipart_update_url(files.files[0].id),
          build_drive_rename_metadata_json(kBackupSettingsDriveName), kBackupSettingsPath,
          "application/json", token);
    }
    return HttpClient().post_multipart_file(
        kDriveUploadEndpoint,
        build_drive_upload_metadata_json(kBackupSettingsDriveName, drive_root_folder_id_),
        kBackupSettingsPath, "application/json", token);
  });
  if (!upload.ok) {
    return false;
  }
  backup_settings_synced_ = tracked_config_.modified;
  save_settings();
  return true;
}

void App::sync_backup_settings_if_dirty() {
  // Local changes ride the next backup upload's network moment - the connection is warm and a
  // modal is already up - instead of lagging a Square press or waiting for the next boot. With
  // nothing changed since the last agreement this costs nothing at all; adopting remote-side
  // changes stays a boot/refresh concern.
  if (tracked_config_.modified != backup_settings_synced_) {
    sync_backup_settings();
  }
}

void App::sync_backup_settings() {
  if (tracked_config_load_failed_ || !google_connected_) {
    return;
  }
  if (drive_root_folder_id_.empty()) {
    drive_root_folder_id_ = find_or_create_drive_folder(kGoogleDriveRootFolderName, "root");
    if (drive_root_folder_id_.empty()) {
      return;
    }
  }
  const std::string list_url =
      std::string(kDriveFilesEndpoint) + "?" +
      build_drive_find_child_by_name_query(drive_root_folder_id_, kBackupSettingsDriveName);
  const HttpResponse list_response = drive_request([&](const std::string &token) {
    return HttpClient().get_json(list_url, token);
  });
  if (!list_response.ok) {
    return;
  }
  const DriveFileList files = parse_drive_file_list(list_response.body);
  if (!files.ok) {
    return;
  }
  const bool local_has_content = !tracked_config_.entries.empty() ||
                                 !tracked_config_.skipped_ids.empty() ||
                                 tracked_config_.modified > 0;
  TrackedFoldersConfig remote;
  if (!files.files.empty()) {
    const std::string content_url =
        std::string(kDriveFilesEndpoint) + "/" + files.files[0].id + "?alt=media";
    const HttpResponse content = drive_request([&](const std::string &token) {
      return HttpClient().get_json(content_url, token);
    });
    if (!content.ok) {
      return;
    }
    TrackedFoldersParseResult parsed = parse_tracked_folders_json(content.body);
    if (!parsed.ok) {
      // A remote nobody can read is not worth adopting; replace it when there is something local.
      if (local_has_content) {
        upload_backup_settings();
      }
      return;
    }
    remote = std::move(parsed.config);
  }
  switch (decide_backup_settings_sync(tracked_config_.modified, local_has_content,
                                      !files.files.empty(), remote.modified,
                                      backup_settings_synced_)) {
  case BackupSettingsSyncAction::InSync:
    return;
  case BackupSettingsSyncAction::Push:
  case BackupSettingsSyncAction::PushConflict:
    upload_backup_settings();
    return;
  case BackupSettingsSyncAction::AdoptConflict: {
    // Only the local loser needs a copy - the remote loser of the push case lives on as a Drive
    // revision. One file, overwritten by the next conflict, so no litter.
    std::string conflict_error;
    write_tracked_folders_json_atomic(kBackupSettingsConflictPath, tracked_config_,
                                      &conflict_error);
    set_status(StatusKind::Info,
               "Backup settings conflict - Drive won, yours kept as backup-settings.conflict.json");
    break;
  }
  case BackupSettingsSyncAction::Adopt:
    set_status(StatusKind::Info, "Backup settings updated from Drive.");
    break;
  }
  std::string write_error;
  if (!write_tracked_folders_json_atomic(kBackupSettingsPath, remote, &write_error)) {
    // Nothing changed in memory yet; the next sync simply tries again.
    return;
  }
  tracked_config_ = std::move(remote);
  backup_settings_synced_ = tracked_config_.modified;
  save_settings();
  apply_tracked_folders();
  apply_sort_and_rebuild();
}

const TrackedFolderEntry *App::applicable_base_entry(const std::string &id) const {
  for (const TrackedFolderEntry &entry : base_config_.entries) {
    if (entry.id != id) {
      continue;
    }
    for (const TrackedPath &path : entry.paths) {
      if (path_is_directory(path.path)) {
        return &entry;
      }
    }
    return nullptr;  // shipped entry, but none of its folders exist here
  }
  return nullptr;
}

bool App::set_entry_data_folders(const std::string &id, const std::string &title,
                                 std::vector<TrackedPath> new_paths) {
  if (tracked_config_load_failed_) {
    // The config could not be read whole this run, so writing it back would truncate it; refuse.
    set_status(StatusKind::Info, "Cannot update backup-settings.json - fix or delete it first.");
    return false;
  }
  const TrackedFolderEntry *base = applicable_base_entry(id);
  // The write is atomic, so a failure leaves the file untouched; restoring the snapshot keeps the
  // in-memory config matching what is still on disk.
  const std::vector<TrackedFolderEntry> snapshot = tracked_config_.entries;
  const long long previous_modified = tracked_config_.modified;
  update_data_folder_override(&tracked_config_, id, title, new_paths,
                              base ? &base->paths : nullptr);
  tracked_config_.modified = static_cast<long long>(std::time(nullptr));
  if (!save_data_folders_config()) {
    tracked_config_.entries = snapshot;
    tracked_config_.modified = previous_modified;
    return false;
  }
  // Times are NOT re-resolved here: a picker visit toggles several folders, and each resolve is a
  // full mtime walk of every extras entry. close_directory_browser does it once, behind a busy
  // frame. The Drive copy catches up at the next boot or refresh - pushing here put two HTTPS
  // round trips on the render thread and lagged every Square press.
  apply_tracked_folders(false);
  return true;
}

void App::apply_tracked_folders(bool resolve_times) {
  // Attach each config entry's extra folders to the app's own record, matched by id. Doing it by
  // id (rather than by path) means an entry survives its app gaining or losing a savedata folder
  // between launches.
  // The composite view: base entries (existence-gated) overlaid by the user's file.
  TrackedFoldersConfig effective;
  effective.entries =
      effective_data_folder_entries(base_config_, tracked_config_, path_is_directory);
  std::unordered_map<std::string, const TrackedFolderEntry *> configured;
  configured.reserve(effective.entries.size());
  for (const TrackedFolderEntry &entry : effective.entries) {
    configured.emplace(entry.id, &entry);
  }

  std::unordered_set<std::string> known_ids;
  known_ids.reserve(saves_.size());
  for (SaveRecord &save : saves_) {
    known_ids.insert(save.id);
    const auto entry = configured.find(save.id);
    // Assigned, not appended: a repeat call after the user excluded a folder has to drop what an
    // earlier call attached, or an excluded folder would keep being backed up until the next launch.
    save.extra_paths = entry == configured.end() ? std::vector<TrackedPath>() : entry->second->paths;
  }

  // An app that keeps everything in ux0:data has no savedata folder for the scan to find, so its
  // row is synthesized from the config here. The app.db pass gives it its real title and icon.
  for (SaveRecord &record : build_orphan_app_records(
           effective,
           [&](const std::string &id) { return known_ids.count(id) != 0; })) {
    saves_.push_back(std::move(record));
  }

  // Times come from the live directories (newest observed file across the savedata folder and every
  // extra). This happens here, after attaching, not in the scanner, which knows nothing about
  // extras. Skipped while the picker is toggling (see set_entry_data_folders); the close pass
  // resolves once for everything.
  if (!resolve_times) {
    return;
  }
  for (SaveRecord &save : saves_) {
    if (!save.extra_paths.empty()) {
      resolve_data_folder_time(&save);
    }
  }
}

void App::queue_selected_save_time_read() {
  // An empty tab has nothing to mount.
  if (selected_save_ >= visible_saves_.size()) {
    return;
  }
  const SaveRecord &save = saves_[visible_saves_[selected_save_]];
  if (!save.save_time_requires_mount ||
      std::find(pending_time_reads_.begin(), pending_time_reads_.end(), save.id) !=
          pending_time_reads_.end()) {
    return;
  }
  pending_time_reads_.push_back(save.id);
}

bool App::drain_pending_time_read() {
  // One request in flight at a time; the next queued save waits for the slot, not for the user.
  if (pending_time_reads_.empty() || mount_work_state_.load() != 0 || mount_work_.async) {
    return false;
  }
  // The focused save is the one showing a spinner, so it is read first however long the queue
  // behind it is; after it, the most recently passed saves come before older ones.
  const std::string focused_id =
      selected_save_ >= visible_saves_.size()
          ? std::string()
          : saves_[visible_saves_[selected_save_]].id;
  auto next = std::find(pending_time_reads_.begin(), pending_time_reads_.end(), focused_id);
  if (next == pending_time_reads_.end()) {
    // Most recently passed first, not oldest first: after a sweep the newest entries are the
    // saves sitting right next to the cursor, so stepping back finds them already resolved.
    // Oldest-first spent the same reads on the sweep's distant starting point instead.
    next = pending_time_reads_.end() - 1;
  }
  const std::string save_id = *next;
  pending_time_reads_.erase(next);

  SaveRecord *save = nullptr;
  for (SaveRecord &record : saves_) {
    if (record.id == save_id && record.save_time_requires_mount) {
      save = &record;
      break;
    }
  }
  if (!save) {
    // Dropped from the list, or already read by a batch or a restore: nothing left to do, and the
    // caller still counts this as work so the next frame moves on to the following entry.
    return true;
  }

  if (mount_worker_thread_ < 0) {
    // No worker: the old inline read, blocking this frame. Rare fallback, still single-threaded.
    resolve_save_time(save);
    return true;
  }
  // The mount, the metadata read, and the fingerprint walk all run on the worker; the main loop
  // keeps drawing and taking input, and complete_async_read applies the result when it lands.
  submit_async_save_time_read(*save);
  return true;
}

void App::write_save_index_when_idle() {
  if (!save_index_dirty_) {
    return;
  }
  // The write blocks the frame the same way a read does, so it gets the same recovery: a press
  // that starts and ends inside it is replayed next frame instead of disappearing.
  const SceCtrlData armed = arm_input_recovery();
  flush_save_index();
  deferred_buttons_ |= buttons_pressed_since(armed);
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
      flush_save_index();
      return false;
    }
    ui_.draw_busy("Reading save times", static_cast<long long>(done),
                  static_cast<long long>(total), kContext, kCancelHint);
    resolve_save_time(&save);
    ++done;
  }
  // Every queued read just happened here, so the queue would only pop entries with nothing left
  // to do.
  pending_time_reads_.clear();
  flush_save_index();
  return true;
}

// Re-locates id within visible_saves_, or falls back to 0 when it is not there (an empty id, or an
// entry that ceased to exist - closing the picker after excluding an orphan app's last folder
// passes the id of a row that is gone).
void App::refocus_selection_by_id(const std::string &id) {
  selected_save_ = 0;
  if (!id.empty()) {
    for (std::size_t i = 0; i < visible_saves_.size(); ++i) {
      if (saves_[visible_saves_[i]].id == id) {
        selected_save_ = i;
        break;
      }
    }
  }
  category_selection_[static_cast<std::size_t>(category_)] = selected_save_;
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
  apply_save_sort(&saves_, sort_mode_, newest_backup_by_folder());
  // Remembered per-tab positions point into the old order; the current save is re-located by id
  // instead so the focus survives the re-sort (the grid window follows it on the next frame).
  category_selection_.fill(0);
  rebuild_visible_saves();
  refocus_selection_by_id(focused_id);
  queue_selected_save_time_read();
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
  // One pass, grid order untouched. Saves checked out of the batch sweep keep their place and
  // their tile: reordering them was the old "hidden" behaviour and it never matched what the
  // label promised.
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
  // A bounds check rather than a modulo, so an empty tab returns nullptr instead of indexing an
  // empty vector. Every caller already treats nullptr as "no save".
  if (selected_save_ >= visible_saves_.size()) {
    return nullptr;
  }
  return &saves_[visible_saves_[selected_save_]];
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
  if (selected_backup_ >= backup_rows_.size()) {
    selected_backup_ = default_backup_row();
  }
}

void App::move_selected_save(int delta) {
  move_selected_save_to(move_selection(selected_save_, visible_saves_.size(), delta));
}

void App::move_selected_save_vertical(int direction) {
  const std::size_t count = visible_saves_.size();
  if (count == 0) {
    return;
  }
  constexpr std::size_t kColumns = static_cast<std::size_t>(kSaveGridColumns);
  const std::size_t index = selected_save_;
  const std::size_t column = index % kColumns;
  std::size_t target;
  if (direction > 0) {
    // Off the bottom of a column, back to its own top. A plain +columns modulo jump landed
    // columns away whenever the last row was partial.
    target = index + kColumns < count ? index + kColumns : column;
  } else {
    // Off the top, down to the column's bottom-most occupied cell - the second-to-last row when
    // the last row does not reach this column.
    target = index >= kColumns ? index - kColumns
                               : column + ((count - 1 - column) / kColumns) * kColumns;
  }
  move_selected_save_to(target);
}

void App::move_selected_save_to(std::size_t target) {
  const std::size_t previous = selected_save_;
  selected_save_ = target < visible_saves_.size() ? target : previous;
  if (selected_save_ != previous) {
    cancel_restore_confirmation();
    cancel_delete_confirmation();
    cancel_duplicate_backup_confirmation();
    // A different save means a different backup list; focus its "New Backup" entry - picked after
    // the refreshes below rebuild the rows, since a homebrew entry seats "Savedata Paths" above it.
    queue_selected_save_time_read();
    refresh_local_backups();
    refresh_remote_backups_view();
    selected_backup_ = default_backup_row();
    // Browsing IS the batch window's point - moving must not close it, and the status line is
    // showing its live count, not a stale per-save message. Outside the window the old message
    // described the previous save, so it goes.
    if (!sync_all_confirmation_pending_) {
      clear_status();
    }
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
    rebuild_visible_saves();
    queue_selected_save_time_read();
    refresh_local_backups();
    refresh_remote_backups_view();
    selected_backup_ = default_backup_row();
    clear_status();
    return;
  }
}

void App::move_selected_backup(int delta) {
  // The batch window hides the backup list behind its panel; scrolling a list that is not on
  // screen would only shuffle state (and cancel the window) blind.
  if (sync_all_confirmation_pending_) {
    return;
  }
  const std::size_t previous = selected_backup_;
  // Every row is selectable, sentinels included - the menu size is simply how many rows there are.
  selected_backup_ = move_selection(selected_backup_, backup_rows_.size(), delta);
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
    const std::vector<ArchiveEntryInfo> entries = compute_save_entries(
        save, &signature_ok, [this](std::uint64_t done, std::uint64_t total) {
          ui_.draw_busy("Checking current save", static_cast<long long>(done),
                        static_cast<long long>(total));
        });
    if (signature_ok && !entries.empty()) {
      const std::string match = matching_backup_name(entries, save.id, local_backups_);
      if (!match.empty()) {
        duplicate_backup_confirmation_pending_ = true;
        // Says why a new backup is redundant; the footer offers "Create New Backup Anyway".
        set_status(StatusKind::Info,
                   status_with_name("No changes since ", display_backup_name(match),
                                                "."));
        return;
      }
    }
  }
  duplicate_backup_confirmation_pending_ = false;

  // One busy frame before the blocking ZIP work, so the screen does not look frozen.
  ui_.draw_busy("Creating backup", 0, -1);
  const LocalSnapshotResult result = create_local_snapshot(save, "", "Creating backup", force_new);
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
                 status_with_name("Created ", display_backup_name(file_name), "."));
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
  // Start deletes the focused backup and nothing else. The "Savedata Paths" row has no Start action:
  // folders are added and removed inside the picker itself, which is the only place that shows which
  // ones an entry actually has.
  if (data_folders_row_focused()) {
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
    set_status(StatusKind::Info, status_with_name("Delete ", display, "?"));
    return;
  }

  if (!delete_confirmation_pending_) {
    restore_confirmation_pending_ = false;
    delete_confirmation_pending_ = true;
    // Card-only is the plain case, so the prompt just names what; a Cloud-only delete keeps its
    // "from the Cloud?" qualifier since removing the only remote copy is the weightier action.
    set_status(StatusKind::Info,
               status_with_name("Delete ", display,
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
  // The folder is worth nothing without backups in it. Unconditional because a Cloud-only delete
  // also drops the stale local companion, and the call does nothing while anything remains.
  remove_local_backup_folder_if_empty(kBackupRoot, selected->id);
  if (selected_backup_ >= backup_rows_.size()) {
    selected_backup_ = backup_rows_.empty() ? 0 : backup_rows_.size() - 1;
  }

  if (local_failed) {
    set_status(StatusKind::Error,
               remote_requested ? "Deleted the Cloud copy, but not the card copy."
                                : status_with_name("Could not delete ", display, "."));
    return;
  }
  if (delete_local && remote_requested) {
    set_status(StatusKind::Success, status_with_name("Deleted ", display, "."));
  } else if (remote_requested) {
    set_status(StatusKind::Success,
               status_with_name("Deleted ", display, " from the Cloud."));
  } else {
    set_status(StatusKind::Success, status_with_name("Deleted ", display, "."));
  }
}

void App::handle_action_button() {
  if (sync_all_confirmation_pending_) {
    // An empty selection has nothing to run; the status line already reads as the nudge.
    if (batch_selected_count() > 0) {
      run_sync_all();
    }
    return;
  }
  // The "Savedata Paths" row is an action, not a backup: opening the picker is what it does.
  if (data_folders_row_focused()) {
    open_directory_browser(false);
    return;
  }
  const SaveRecord *selected = selected_save_record();
  if (!selected) {
    set_status(StatusKind::Info, "No save selected.");
    return;
  }
  // One context-sensitive action button: the "New Backup" entry creates a snapshot, a backup
  // entry restores it (with a second press to confirm).
  if (new_backup_row_focused()) {
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
    set_status(StatusKind::Info, status_with_name("Restore ", row.display_name(), "?"));
    return;
  }

  const SaveRecord &save = *selected;
  // The worker may be mid-mount of this very folder for a queued read; restoring would rewrite
  // the files out from under it. Land and apply the in-flight read before touching the save.
  complete_async_read(true);
  // A card copy restores directly; a cloud-only snapshot downloads into the local backup folder
  // first (and stays there, so the row becomes card + Drive).
  std::string archive_path = local_backup_archive_path(kBackupRoot, save.id, backup_name);
  const bool remote_restore = !row.has_local();

  // Safety net: snapshot the current save before overwriting it, unless some local backup
  // already holds exactly this content (compared by per-file path, size, and CRC32, because
  // file timestamps change on every restore and cannot be trusted).
  ui_.draw_busy("Checking current save", 0, -1);
  bool signature_ok = false;
  const std::vector<ArchiveEntryInfo> current_entries = compute_save_entries(
      save, &signature_ok, [this](std::uint64_t done, std::uint64_t total) {
        ui_.draw_busy("Checking current save", static_cast<long long>(done),
                      static_cast<long long>(total));
      });
  if (signature_ok && !current_entries.empty()) {
    const bool already_backed_up =
        !matching_backup_name(current_entries, save.id, local_backups_).empty();
    if (!already_backed_up) {
      ui_.draw_busy("Backing up current save", 0, -1);
      const LocalSnapshotResult auto_result =
          create_local_snapshot(save, " auto", "Backing up current save");
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
      // ensure_parent_directory created the folder for this download; do not leave it behind empty.
      remove_local_backup_folder_if_empty(kBackupRoot, save.id);
      restore_confirmation_pending_ = false;
      set_status(StatusKind::Error, "Cloud download failed.");
      return;
    }
    refresh_local_backups();
  }

  ui_.draw_busy("Restoring save", 0, -1);
  RestoreRequest restore_request;
  restore_request.archive_path = archive_path;
  // Whether an archive is flat or carries prefixes is a property of that archive, not of today's
  // config, so the sidecar decides - and it is read for every restore, not just entries that
  // currently have extras. That matters when folders were included, a backup was taken, and the
  // folders were then excluded: the archive is still prefixed, and consulting the live entry alone
  // would extract its prefixes as literal subfolders into the savedata directory.
  //
  // The archive is already on the card here (a card copy, or the cloud-only copy downloaded just
  // above), and its sidecar is read off the card with no Drive round-trip. A cloud-only backup's
  // sidecar is never downloaded, so its targets come up empty and restore falls back to the entry.
  bool sidecar_targets_unsafe = false;
  const std::string metadata_path = local_backup_metadata_path(kBackupRoot, save.id, backup_name);
  const SaveMetadataJsonResult sidecar = read_save_metadata_json(metadata_path);
  const std::vector<TrackedPath> &recorded = sidecar.metadata.tracked_targets;
  std::vector<TrackedPath> extra_targets;
  if (sidecar.ok && !recorded.empty() && tracked_targets_are_safe(recorded)) {
    extra_targets = recorded;
  } else {
    // A sidecar that lists targets yet fails validation is corrupt or hand-tampered: do not honor
    // it, but still restore to the current entry's folders rather than block the user. With no
    // sidecar at all this is simply what the entry looks like now - an empty list for an ordinary
    // save, which is the flat restore path below.
    sidecar_targets_unsafe = sidecar.ok && !recorded.empty();
    extra_targets = archive_layout_for_record(save).extra_targets;
  }
  if (extra_targets.empty()) {
    restore_request.destination_path = save.path;
  } else {
    // The savedata destination is supplied here from the live entry, never read from the sidecar,
    // so a tampered sidecar can only ever aim a directory clear inside ux0:data.
    for (const TrackedPath &target : restore_targets_for_backup(save.path, extra_targets)) {
      restore_request.targets.push_back({target.prefix, target.path, target.is_file});
    }
  }
  restore_request.progress = [this](std::uint64_t done, std::uint64_t total) {
    ui_.draw_busy("Restoring save", static_cast<long long>(done),
                  static_cast<long long>(total));
  };
  const RestoreResult result = restore_backup_archive(restore_request);
  restore_confirmation_pending_ = false;
  if (result.ok) {
    // The live folder now holds different content; drop the cached time and re-read so the grid
    // does not keep showing the pre-restore save time.
    invalidate_save_time(save);
    queue_selected_save_time_read();
    if (sidecar_targets_unsafe) {
      set_status(StatusKind::Info,
                 status_with_name("Restored ", display_backup_name(backup_name),
                                  ", but its saved folder locations were invalid and ignored."));
    } else {
      set_status(StatusKind::Success,
                 status_with_name(
                     remote_restore ? "Downloaded and restored " : "Restored ",
                     display_backup_name(backup_name), "."));
    }
  } else {
    set_status(StatusKind::Error, "Restore failed: " + result.error);
  }
}

void App::invalidate_save_time(const SaveRecord &restored) {
  if (!restored.extra_paths.empty()) {
    // Entries with extra folders never live in the save index and never mount. Re-resolve their
    // time from the (now restored) live directories, the same way attaching does.
    for (SaveRecord &record : saves_) {
      if (record.id == restored.id && !record.extra_paths.empty()) {
        resolve_data_folder_time(&record);
        break;
      }
    }
    return;
  }
  // Belt and braces behind handle_restore's complete_async_read: should a read for this save
  // still be in flight, its result describes the pre-restore folder and must not be applied.
  if (mount_work_.async && mount_work_.async_save_id == restored.id) {
    mount_work_.discard = true;
  }
  // Only the time resets, back to "never resolved". The title fields and old fingerprint stay:
  // a stale fingerprint makes the next scan re-read an sfo-derived title and keeps an app-db
  // one, exactly as the split files behaved.
  const auto entry = save_index_.entries.find(restored.id);
  if (entry != save_index_.entries.end() && entry->second.time_resolved) {
    entry->second.time_resolved = false;
    entry->second.has_time = false;
    entry->second.saved_at = {};
    save_index_dirty_ = true;
  }
  flush_save_index();
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
    google_credentials_error_ = GoogleSetupPrompt::MissingFile;
    set_status(StatusKind::Error, "No Google client set up - see docs/google-drive-setup.md");
    return false;
  }

  google_credentials_ = parse_google_client_credentials(json);
  if (!google_credentials_.ok) {
    google_credentials_error_ = GoogleSetupPrompt::InvalidFile;
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
      sync_backup_settings();
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
    // The one-line status truncates exactly where the guide path starts; a modal with the guide
    // QR explains the setup instead.
    clear_status();
    google_setup_prompt_ = google_credentials_error_;
    return;
  }

  BusyLabelScope busy("Contacting Google", /*indeterminate=*/true);
  HttpClient http;
  const HttpResponse response =
      http.post_form(kGoogleDeviceCodeEndpoint,
                     build_device_code_request_body(google_credentials_.client_id));
  if (!response.ok) {
    // A definitive 4xx here means Google rejected the client credentials themselves (deleted or
    // mistyped OAuth client). Transport failures and server errors keep the one-line status, so
    // an offline console never blames the credentials file.
    if (response.status == 400 || response.status == 401 || response.status == 403) {
      clear_status();
      google_setup_prompt_ = GoogleSetupPrompt::RejectedByGoogle;
      return;
    }
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
  if (selected_backup_ >= backup_rows_.size()) {
    selected_backup_ = default_backup_row();
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
  // A homebrew entry gets a "Savedata Paths" row right above "New Backup". Putting the picker in the
  // list rather than on a button means it is visible without knowing it exists, and it costs no
  // footer space - the grid's footer has none left. Default focus still lands on "New Backup"
  // (default_backup_row), so switching titles keeps its familiar landing spot.
  const SaveRecord *save = selected_save_record();
  if (save && classify_save(*save) == SaveCategory::Homebrew && !backup_rows_.empty()) {
    backup_rows_.insert(backup_rows_.begin(),
                        BackupRow::data_folders_row(save->extra_paths.size()));
  }
}

std::size_t App::default_backup_row() const {
  // The "New Backup" row, wherever it sits - the landing spot whenever focus resets.
  for (std::size_t i = 0; i < backup_rows_.size(); ++i) {
    if (backup_rows_[i].new_backup) {
      return i;
    }
  }
  return 0;
}

bool App::new_backup_row_focused() const {
  return selected_backup_ < backup_rows_.size() && backup_rows_[selected_backup_].new_backup;
}

std::size_t App::backup_count() const {
  // Snapshot rows only. "New Backup" and "Savedata Paths" stand for actions, not backups, so a count
  // taken by subtracting a fixed number of leading rows would drift as sentinels come and go.
  std::size_t count = 0;
  for (const BackupRow &row : backup_rows_) {
    if (!row.is_sentinel()) {
      ++count;
    }
  }
  return count;
}

const BackupRow *App::selected_backup_row() const {
  // Sentinels read as "no row", which is what makes every backup action - restore, delete, label,
  // transfer - refuse them without each having to know they exist.
  if (selected_backup_ >= backup_rows_.size() || backup_rows_[selected_backup_].is_sentinel()) {
    return nullptr;
  }
  return &backup_rows_[selected_backup_];
}

bool App::data_folders_row_focused() const {
  return selected_backup_ < backup_rows_.size() && backup_rows_[selected_backup_].data_folders;
}

std::string App::selected_backup_name() const {
  const BackupRow *row = selected_backup_row();
  return row ? row->primary_name() : std::string();
}

// Re-locate a snapshot after the rows were rebuilt (new sibling, rename, upload); matching by
// identity keeps the focus on the same snapshot whichever side its name came from.
void App::focus_backup_row_by_identity(const std::string &backup_name) {
  const std::string identity = backup_identity(backup_name);
  for (std::size_t i = 0; i < backup_rows_.size(); ++i) {
    if (backup_rows_[i].is_sentinel()) {
      continue;
    }
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
    // A Drive-only row may have had its folder created just above; do not leave it behind empty.
    remove_local_backup_folder_if_empty(kBackupRoot, save.id);
    if (error) {
      *error = "Cloud download failed.";
    }
    return false;
  }
  std::string publish_error;
  if (!publish_backup_download(temporary_path, archive_path, &publish_error)) {
    // publish_backup_download already removed the temporary. When it failed because the backup
    // already exists the folder is not empty and this does nothing.
    remove_local_backup_folder_if_empty(kBackupRoot, save.id);
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
               status_with_name("Downloaded ", display_backup_name(row.remote_name),
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
  // One bar for the whole operation: the folder lookups hold it at zero, the archive fills it,
  // and the companion upload holds it full. Without a readable size the hook reports as before.
  bool zip_size_ok = false;
  const std::uint64_t zip_bytes = archive_file_size(
      local_backup_archive_path(kBackupRoot, selected->id, backup_name), &zip_size_ok);
  TransferBudgetScope budget(zip_size_ok ? static_cast<long long>(zip_bytes) : 0);
  const BackupUploadResult uploaded = upload_local_backup(*selected, backup_name);
  if (!uploaded.ok) {
    return;
  }
  sync_backup_settings_if_dirty();
  refresh_remote_backups_view();
  // The rebuild may have shifted the rows; keep the selection on the file this action was about.
  focus_backup_row_by_identity(backup_name);
  if (uploaded.metadata_warning) {
    set_status(StatusKind::Info, "Backup uploaded, but slot details were not synced.");
  } else {
    set_status(StatusKind::Success,
               status_with_name("Uploaded ", display_backup_name(backup_name),
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
      // The cap repeats the declaration default so the progress callback can follow it. The label
      // matches the modal the details view already put on screen; the mount after the extraction
      // stays a pulse.
      const RestoreResult extracted = extract_backup_archive_for_inspection(
          archive_path, inspection.path(), 512ULL * 1024ULL * 1024ULL,
          [this](std::uint64_t done, std::uint64_t total) {
            ui_.draw_busy("Loading save details", static_cast<long long>(done),
                          static_cast<long long>(total));
          });
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
  // Discarding must also drop a folder that ensure_parent_directory created just for this
  // download; when the save has no card backups the folder would otherwise linger empty. Kept
  // in-function because the rename-failure path below still reports success to the caller.
  const auto discard_download = [&] {
    std::remove(temporary_path.c_str());
    remove_local_backup_folder_if_empty(kBackupRoot, save.id);
  };
  const std::string url = std::string(kDriveFilesEndpoint) + "/" +
                          form_url_encode(sidecar.id) + "?alt=media";
  const HttpResponse downloaded = drive_request([&](const std::string &token) {
    return HttpClient().download_file(url, temporary_path, token);
  });
  if (!downloaded.ok) {
    discard_download();
    return {false, {}, {}, "slot details download failed"};
  }
  SaveMetadataJsonResult metadata = read_save_metadata_json(temporary_path);
  if (!save_metadata_is_usable(metadata, expected_identity)) {
    discard_download();
    metadata.ok = false;
    metadata.error = "slot details do not match this backup";
    return metadata;
  }
  if (!upgrade_legacy_metadata_file(temporary_path, expected_identity, &metadata)) {
    discard_download();
    return metadata;
  }
  if (std::rename(temporary_path.c_str(), metadata_path.c_str()) != 0) {
    // The details were downloaded and validated; only caching them for next time failed. Return
    // the in-memory metadata so the screen can still show it instead of "unavailable".
    discard_download();
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

void App::request_save_details() {
  // The "Savedata Paths" row has no details of its own - Triangle opens the picker instead, an
  // unhinted shortcut that mirrors Triangle silently closing it from inside.
  if (data_folders_row_focused()) {
    open_directory_browser(false);
    return;
  }
  // Entering details silently drops transient overview feedback: a stale status line is
  // irrelevant on the new screen, and a pending confirmation must not carry a primed second
  // press across screens.
  restore_confirmation_pending_ = false;
  duplicate_backup_confirmation_pending_ = false;
  clear_status();
  const BackupRow *row = selected_backup_row();
  const SaveRecord *save = selected_save_record();
  // The live-save ("New Backup") row shows a time still being read through a mount. Opening now
  // would race that background read and the press could be swallowed by it, so defer the open until
  // the time lands (the main loop tick fires it, and any other input cancels it). Backup rows carry
  // their own metadata, so they open immediately.
  if (new_backup_row_focused() && save && save->save_time_requires_mount) {
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

  // The busy modal appears only ahead of real work - a mount, an archive inspection, or a network
  // fetch. A details open that reads a cached companion is instant and must not flash a modal.
  slot_details_ = {};
  slot_details_.game_title = save.display_name;
  slot_details_.data_folders_row = data_folders_row_focused();
  // Shared by both branches below (the live row and a snapshot describe the same entry), so the
  // details screen can list the extra folders regardless of which one is inspected. Left empty by
  // the reset above for every entry that has none; a snapshot with its own sidecar record
  // replaces this with what the archive actually holds, further down.
  for (const TrackedPath &extra : save.extra_paths) {
    slot_details_.extra_paths.push_back({extra.path, 0, false, extra.is_file, 0});
  }
  if (!selected_row) {
    // "New Backup" represents the live save. Resolve its details directly without creating an
    // archive or JSON companion; this is a read-only preview of what the next backup would use.
    slot_details_.snapshot_name = "Current Save";
    if (save.platform != SavePlatform::Psp) {
      ui_.draw_busy("Loading save details", 0, -1);
    }
    slot_details_.metadata =
        save.extra_paths.empty()
            ? resolve_live_save_metadata(save.path, {}, save.platform != SavePlatform::Psp,
                                         mount_bridge_ready_)
            : resolve_tracked_metadata(save_path_list(save), {});
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
    // An entry with extras sums its savedata folder and every extra, which is what its next backup
    // would hold; unknown only when all of those directories are unreadable.
    if (save.extra_paths.empty()) {
      bool save_size_ok = false;
      const std::uint64_t save_bytes = compute_folder_size(save.path, &save_size_ok);
      if (save_size_ok) {
        slot_details_.save_bytes = save_bytes;
        slot_details_.save_bytes_known = true;
      }
    } else {
      std::uint64_t total = 0;
      bool any_ok = false;
      if (!save.path.empty()) {
        bool path_ok = false;
        const std::uint64_t bytes = compute_folder_size(save.path, &path_ok);
        if (path_ok) {
          total += bytes;
          any_ok = true;
        }
      }
      // Each extra folder is measured on its own so the folder list can print per-folder sizes;
      // the total still sums savedata and every extra, which is what the next backup would hold.
      for (std::size_t i = 0; i < save.extra_paths.size(); ++i) {
        bool path_ok = false;
        std::uint64_t bytes = 0;
        std::size_t files = 0;
        if (save.extra_paths[i].is_file) {
          struct stat file_info {};
          path_ok = stat(save.extra_paths[i].path.c_str(), &file_info) == 0 &&
                    S_ISREG(file_info.st_mode);
          bytes = path_ok ? static_cast<std::uint64_t>(file_info.st_size) : 0;
        } else {
          bytes = compute_folder_size(save.extra_paths[i].path, &path_ok, &files);
        }
        if (path_ok) {
          total += bytes;
          any_ok = true;
          slot_details_.extra_paths[i].bytes = bytes;
          slot_details_.extra_paths[i].bytes_known = true;
          slot_details_.extra_paths[i].files = files;
        }
      }
      if (any_ok) {
        slot_details_.save_bytes = total;
        slot_details_.save_bytes_known = true;
      }
    }
    slot_details_.open = true;
    return;
  }

  const BackupRow row = *selected_row;
  slot_details_.snapshot_name = row.primary_name();
  slot_details_.snapshot_on_card = row.has_local();
  slot_details_.snapshot_in_cloud = row.has_remote();
  slot_details_.google_connected = google_connected_;

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
    if (!already_cached) {
      // Recovery reads the ZIP and may extract and mount it; a cached companion opens instantly.
      ui_.draw_busy("Loading save details", 0, -1);
    }
    metadata = ensure_local_backup_metadata(save, row.local_name);
    if (metadata.ok && !already_cached && row.has_remote()) {
      // Replace an unusable Drive companion too: local recovery just produced a healthy one.
      repair_remote_backup_metadata(save, row, true);
    }
  }

  // A missing local companion may still exist on Drive. Fetch only this one small JSON file when
  // the user asks for details; the startup index remains ZIP-only and fast.
  if (!metadata.ok && row.has_remote() && network_connected_) {
    ui_.draw_busy("Loading save details", 0, -1);
    if (ensure_google_access_token()) {
      metadata = download_remote_backup_metadata(save, row.remote_name,
                                                 remote_file_id_for(row.remote_name));
    }
  }

  const bool usable_metadata =
      save_metadata_is_usable(metadata, backup_identity(row.primary_name()));

  if (usable_metadata) {
    slot_details_.metadata = std::move(metadata.metadata);
    if (!slot_details_.metadata.tracked_targets.empty()) {
      // A snapshot lists the folders its own sidecar recorded - what the archive actually holds -
      // not the entry's current set, which may have changed since the backup was made. Per-folder
      // sizes come from the archive's central directory, summed under each target's prefix; a
      // Cloud-only copy has no local file to read, so its paths list without sizes.
      slot_details_.extra_paths.clear();
      for (const TrackedPath &target : slot_details_.metadata.tracked_targets) {
        slot_details_.extra_paths.push_back({target.path, 0, false, target.is_file, 0});
      }
      std::vector<ArchiveEntryInfo> archive_entries;
      if (row.has_local() &&
          read_archive_central_directory(
              local_backup_archive_path(kBackupRoot, save.id, row.local_name),
              &archive_entries)) {
        for (std::size_t i = 0; i < slot_details_.metadata.tracked_targets.size(); ++i) {
          const std::string &prefix = slot_details_.metadata.tracked_targets[i].prefix;
          if (prefix.empty()) {
            continue;
          }
          std::uint64_t bytes = 0;
          std::size_t files = 0;
          for (const ArchiveEntryInfo &entry : archive_entries) {
            if (entry.path.size() > prefix.size() + 1 &&
                entry.path.compare(0, prefix.size(), prefix) == 0 &&
                entry.path[prefix.size()] == '/') {
              bytes += entry.size;
              ++files;
            }
          }
          slot_details_.extra_paths[i].bytes = bytes;
          slot_details_.extra_paths[i].bytes_known = true;
          slot_details_.extra_paths[i].files = files;
        }
      }
    }
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
  // An allocation failure anywhere in the upload chain used to escape as an uncaught bad_alloc
  // and take the whole app down with a crash dump; a clean per-file failure lets a sync batch
  // carry on with the remaining games.
  try {
    return upload_local_backup_impl(save, backup_name);
  } catch (const std::bad_alloc &) {
    set_status(StatusKind::Error, "Out of memory while uploading.");
    return {};
  }
}

BackupUploadResult App::upload_local_backup_impl(const SaveRecord &save,
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
  // The archive's bytes are done; folding them into the budget base keeps the single-operation
  // bar full through the companion upload and any index refresh. A no-op when no budget is open.
  bool uploaded_size_ok = false;
  const std::uint64_t uploaded_bytes = archive_file_size(archive_path, &uploaded_size_ok);
  if (uploaded_size_ok) {
    TransferBudgetScope::advance(static_cast<long long>(uploaded_bytes));
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
                 : status_with_name("Labeled ", display_backup_name(new_name), "."));
}

std::string App::browser_start_path(const SaveRecord &save) const {
  // app.db carries no ux0:data path for an app - verified against the device, where "ux0:data"
  // appears nowhere in it - so where to open is necessarily a guess. Best evidence first.
  if (!save.extra_paths.empty()) {
    // Beside a folder this entry already has, so adding RetroArch's savestates right after its
    // savefiles opens in ux0:data/retroarch instead of back at the root.
    const std::string &first = save.extra_paths.front().path;
    const std::size_t slash = first.rfind('/');
    if (slash != std::string::npos && slash > 0) {
      const std::string parent = first.substr(0, slash);
      if (path_is_directory(parent)) {
        return parent;
      }
    }
  }
  // Otherwise the closest name match between the app's title and the ux0:data children. Only the
  // starting directory - the user can always navigate out to the root.
  const std::string match =
      best_data_folder_match(save.display_name, list_child_directories(kUserDataRoot));
  return match.empty() ? std::string(kUserDataRoot) : std::string(kUserDataRoot) + "/" + match;
}

void App::open_directory_browser(bool from_details) {
  const SaveRecord *selected = selected_save_record();
  if (!selected) {
    set_status(StatusKind::Info, "No save selected.");
    return;
  }
  if (classify_save(*selected) != SaveCategory::Homebrew) {
    set_status(StatusKind::Info, "Savedata paths are for homebrew entries.");
    return;
  }
  abort_browser_size_walk();
  browser_size_cache_.clear();
  browser_size_queue_.clear();
  directory_browser_ = {};
  directory_browser_.open = true;
  directory_browser_.entry_id = selected->id;
  directory_browser_.entry_name = selected->display_name;
  directory_browser_.current_path = browser_start_path(*selected);
  directory_browser_.start_path = directory_browser_.current_path;
  directory_browser_.return_to_details = from_details;
  reload_browser_rows();
  clear_status();
}

void App::close_directory_browser() {
  const std::string entry_id = directory_browser_.entry_id;
  const bool return_to_details = directory_browser_.return_to_details;
  const bool changed = directory_browser_.changed;
  abort_browser_size_walk();
  browser_size_queue_.clear();
  directory_browser_.open = false;
  directory_browser_.rows.clear();
  directory_browser_.large_confirm_pending = false;
  pending_browser_size_frames_ = -1;
  // Drop any browser feedback so a stale prompt does not follow back out; the caller sets its own
  // status afterwards if it has something to say.
  clear_status();

  // Path changes were applied to saves_ as they happened, but the entry's save time and the grid
  // order were deliberately left alone (a resolve is a full mtime walk - too slow per toggle).
  // Settle both now, once, behind a busy frame - and only when this visit actually changed
  // something: a browse-only visit costs nothing on the way out. Only the edited entry needs a
  // resolve; every other entry's paths are exactly as the boot pass left them.
  if (changed) {
    ui_.draw_busy("Updating save times", 0, -1);
    for (SaveRecord &save : saves_) {
      if (save.id == entry_id && !save.extra_paths.empty()) {
        resolve_data_folder_time(&save);
      }
    }
    apply_sort_and_rebuild();
  }

  const SaveRecord *current = selected_save_record();
  if (current && current->id == entry_id) {
    // Land back on the "Savedata Paths" row the picker was opened from, with its folder count now
    // updated, rather than kicking the focus up to "New Backup" - and when the visit started in
    // Save Details, reopen it, which rebuilds its folder list and sizes to show the changes.
    for (std::size_t i = 0; i < backup_rows_.size(); ++i) {
      if (backup_rows_[i].data_folders) {
        selected_backup_ = i;
        break;
      }
    }
    if (return_to_details) {
      open_save_details();
    }
  } else {
    // The entry ceased to exist - its last folder was excluded and it had no savedata of its own -
    // so the grid, refocused by the sort above, is the destination, starting at "New Backup".
    selected_backup_ = default_backup_row();
  }
}

// True when descendant sits strictly inside ancestor (shares its prefix at a "/" boundary).
bool path_is_inside(const std::string &descendant, const std::string &ancestor) {
  return descendant.size() > ancestor.size() + 1 &&
         descendant.compare(0, ancestor.size(), ancestor) == 0 &&
         descendant[ancestor.size()] == '/';
}

void App::reload_browser_rows(bool keep_selection) {
  // Folders already backed up, pulled straight from saves_ so the tags always match reality. Split
  // by owner: this entry's own folders can be excluded again from here, another entry's cannot -
  // one folder belongs to one app, and excluding it here would silently edit an entry the user is
  // not looking at.
  std::unordered_set<std::string> mine;
  std::unordered_set<std::string> others;
  for (const SaveRecord &save : saves_) {
    const bool is_target = save.id == directory_browser_.entry_id;
    for (const TrackedPath &extra : save.extra_paths) {
      (is_target ? mine : others).insert(extra.path);
    }
  }
  // Re-tagging after a toggle must not move the cursor - the folder list is the same, only its
  // tags changed. Drilling in or out passes false and starts at the top. A size walk in flight is
  // left running: its result lands in the cache wherever the user is by then, and the sizing flag
  // is re-marked below if its row is still on screen.
  const std::size_t previous_selected = directory_browser_.selected;
  directory_browser_.rows.clear();
  directory_browser_.selected = 0;
  directory_browser_.large_confirm_pending = false;
  if (directory_browser_.current_path != kUserDataRoot) {
    // A visible way up, on top where every file browser puts it; Cross on it climbs a level.
    DirectoryBrowserState::Row up;
    up.name = "..";
    up.parent_link = true;
    directory_browser_.rows.push_back(std::move(up));
  }
  for (const BrowserChild &child_entry : list_browser_children(directory_browser_.current_path)) {
    DirectoryBrowserState::Row row;
    row.name = child_entry.name;
    row.is_file = child_entry.is_file;
    const std::string child = directory_browser_.current_path + "/" + child_entry.name;
    row.tracked_here = mine.count(child) != 0;
    if (!row.tracked_here) {
      // Inside an included folder means backed up through it; nesting a second inclusion in
      // either direction would archive the overlap twice and restore it twice.
      for (const std::string &included : mine) {
        if (path_is_inside(child, included)) {
          row.covered_by = included.substr(included.rfind('/') + 1);
          break;
        }
      }
    }
    if (!row.tracked_here && row.covered_by.empty()) {
      row.tracked_elsewhere = others.count(child) != 0;
      if (!row.tracked_elsewhere) {
        for (const std::string &taken : others) {
          if (path_is_inside(child, taken)) {
            row.tracked_elsewhere = true;
            break;
          }
        }
      }
    }
    // Sizes survive re-tags and revisits through the per-visit cache, so Square never wipes what
    // was already measured. A file's size arrived with its dirent and needs neither.
    if (row.is_file) {
      row.size_known = true;
      row.size_bytes = child_entry.size;
    } else {
      const auto cached = browser_size_cache_.find(child);
      if (cached != browser_size_cache_.end()) {
        row.size_known = true;
        row.size_bytes = cached->second;
      }
    }
    directory_browser_.rows.push_back(std::move(row));
  }
  refresh_browser_sizing_marks();
  if (keep_selection && previous_selected < directory_browser_.rows.size()) {
    directory_browser_.selected = previous_selected;
  }
  directory_browser_.included_count = mine.size();
  schedule_browser_size_resolve();
}

void App::browser_jump_included(int delta) {
  clear_status();
  // L/R hop across the entry's included folders wherever they live, changing directory when the
  // next one is elsewhere. From an unincluded row, R starts at the first and L at the last.
  const SaveRecord *record = nullptr;
  for (const SaveRecord &save : saves_) {
    if (save.id == directory_browser_.entry_id) {
      record = &save;
      break;
    }
  }
  if (!record || record->extra_paths.empty()) {
    return;
  }
  const std::vector<TrackedPath> &paths = record->extra_paths;
  int current = -1;
  if (directory_browser_.selected < directory_browser_.rows.size() &&
      !directory_browser_.rows[directory_browser_.selected].parent_link) {
    const std::string focused = directory_browser_.current_path + "/" +
                                directory_browser_.rows[directory_browser_.selected].name;
    for (std::size_t i = 0; i < paths.size(); ++i) {
      if (paths[i].path == focused) {
        current = static_cast<int>(i);
        break;
      }
    }
  }
  const int count = static_cast<int>(paths.size());
  const int target = current < 0 ? (delta > 0 ? 0 : count - 1)
                                 : (current + delta + count) % count;
  const std::string &path = paths[static_cast<std::size_t>(target)].path;
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    return;
  }
  const std::string parent = path.substr(0, slash);
  const std::string name = path.substr(slash + 1);
  if (parent != directory_browser_.current_path) {
    directory_browser_.current_path = parent;
    reload_browser_rows();
  }
  for (std::size_t i = 0; i < directory_browser_.rows.size(); ++i) {
    if (!directory_browser_.rows[i].parent_link && directory_browser_.rows[i].name == name) {
      directory_browser_.selected = i;
      break;
    }
  }
  schedule_browser_size_resolve();
}

void App::browser_go_up() {
  clear_status();
  // Names never contain a separator, so the last '/' is always the parent boundary; callers only
  // reach this strictly below ux0:data. The cursor lands on the folder just left, so climbing to
  // pick a sibling does not start the hunt over from the top.
  const std::string from = directory_browser_.current_path.substr(
      directory_browser_.current_path.rfind('/') + 1);
  directory_browser_.current_path.erase(directory_browser_.current_path.rfind('/'));
  reload_browser_rows();
  for (std::size_t i = 0; i < directory_browser_.rows.size(); ++i) {
    if (directory_browser_.rows[i].name == from) {
      directory_browser_.selected = i;
      break;
    }
  }
  schedule_browser_size_resolve();
}

void App::schedule_browser_size_resolve() {
  // Debounce until the focused row settles, so scrolling the list does not size every folder it
  // passes. A walk already running is NOT abandoned - its progress finishes into the cache; rows
  // the cursor rests on meanwhile join a queue behind it.
  if (directory_browser_.selected >= directory_browser_.rows.size() ||
      directory_browser_.rows[directory_browser_.selected].size_known ||
      directory_browser_.rows[directory_browser_.selected].parent_link) {
    pending_browser_size_frames_ = -1;
    return;
  }
  pending_browser_size_frames_ = kBrowserSizeDelayFrames;
}

void App::start_browser_size_walk() {
  const auto begin_walk = [&](const std::string &path) {
    browser_size_walk_ = {};
    browser_size_walk_.active = true;
    browser_size_walk_.target = path;
    browser_size_walk_.stack.push_back({path, nullptr, 0});
  };
  if (browser_size_walk_.active) {
    // One walk at a time. The settled row joins the queue instead, measured in hover order once
    // the current walk finishes.
    if (directory_browser_.selected < directory_browser_.rows.size()) {
      const DirectoryBrowserState::Row &row = directory_browser_.rows[directory_browser_.selected];
      if (!row.size_known && !row.parent_link) {
        const std::string path = directory_browser_.current_path + "/" + row.name;
        if (path != browser_size_walk_.target &&
            std::find(browser_size_queue_.begin(), browser_size_queue_.end(), path) ==
                browser_size_queue_.end()) {
          browser_size_queue_.push_back(path);
        }
      }
    }
    refresh_browser_sizing_marks();
    return;
  }
  // The focused row goes first; otherwise take the oldest hovered folder still worth measuring.
  bool started = false;
  if (directory_browser_.selected < directory_browser_.rows.size()) {
    DirectoryBrowserState::Row &row = directory_browser_.rows[directory_browser_.selected];
    if (!row.size_known && !row.parent_link) {
      begin_walk(directory_browser_.current_path + "/" + row.name);
      started = true;
    }
  }
  while (!started && !browser_size_queue_.empty()) {
    const std::string path = browser_size_queue_.front();
    browser_size_queue_.erase(browser_size_queue_.begin());
    // A queued folder can already be in the cache: an earlier walk rolled it up as a subfolder.
    if (browser_size_cache_.find(path) == browser_size_cache_.end()) {
      begin_walk(path);
      started = true;
    }
  }
  refresh_browser_sizing_marks();
}

void App::refresh_browser_sizing_marks() {
  // The spinner means "this folder's total is on its way": the active walk's target, every queued
  // folder, and every row INSIDE one of those - the depth-first walk caches each subtree it
  // unwinds through, so drilling into a folder mid-measurement shows its children spinning and
  // then filling in as their branches complete.
  const auto scheduled = [&](const std::string &path) {
    if (browser_size_walk_.active &&
        (browser_size_walk_.target == path || path_is_inside(path, browser_size_walk_.target))) {
      return true;
    }
    for (const std::string &queued : browser_size_queue_) {
      if (queued == path || path_is_inside(path, queued)) {
        return true;
      }
    }
    return false;
  };
  for (DirectoryBrowserState::Row &row : directory_browser_.rows) {
    if (row.parent_link || row.size_known) {
      row.sizing = false;
      continue;
    }
    row.sizing = scheduled(directory_browser_.current_path + "/" + row.name);
  }
}

void App::abort_browser_size_walk() {
  for (BrowserSizeWalkFrame &frame : browser_size_walk_.stack) {
    if (frame.dir) {
      closedir(frame.dir);
    }
  }
  browser_size_walk_ = {};
  for (DirectoryBrowserState::Row &row : directory_browser_.rows) {
    row.sizing = false;
  }
}

// Fills the on-screen row for path when its parent directory is the one being browsed.
void App::fill_browser_row_size(const std::string &path, std::uint64_t bytes) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos || path.substr(0, slash) != directory_browser_.current_path) {
    return;
  }
  const std::string name = path.substr(slash + 1);
  for (DirectoryBrowserState::Row &row : directory_browser_.rows) {
    if (row.name == name) {
      row.size_bytes = bytes;
      row.size_known = true;
      row.sizing = false;
      break;
    }
  }
}

void App::advance_browser_size_walk() {
  if (!browser_size_walk_.active) {
    return;
  }
  // One frame's slice, budgeted by time and checked EVERY step - a single directory operation on
  // the card's FAT stack can cost more than a millisecond, so even a 16-step check window let a
  // slice overrun the frame and eat button presses. 3 ms leaves the bulk of a 60 fps frame free;
  // the step cap is only a backstop for a clock hiccup.
  //
  // No stat() at all: the Vita's readdir already returns each entry's SceIoStat in d_stat, which
  // halves the syscalls and skips the expensive path-based lookup that made big trees crawl.
  //
  // The walk is depth-first with one open directory per level. When a directory finishes, its
  // total (own files + finished children) is cached and added to its parent - so every subfolder's
  // size falls out of the same walk, deepest branches first, and stepping into a freshly sized
  // folder finds its children already measured.
  constexpr SceUInt64 kFrameBudgetUs = 3000;
  constexpr int kMaxStepsPerFrame = 4096;
  const SceUInt64 slice_start = sceKernelGetProcessTimeWide();
  BrowserSizeWalk &walk = browser_size_walk_;
  for (int step = 0; step < kMaxStepsPerFrame; ++step) {
    if (step != 0 && sceKernelGetProcessTimeWide() - slice_start >= kFrameBudgetUs) {
      return;
    }
    if (walk.stack.empty()) {
      // Whole target finished; its total was cached by the final pop below. Chain to whatever the
      // cursor wants next (focused row first, then the hover queue).
      abort_browser_size_walk();
      start_browser_size_walk();
      return;
    }
    BrowserSizeWalkFrame &frame = walk.stack.back();
    if (!frame.dir) {
      frame.dir = opendir(frame.path.c_str());
      if (!frame.dir) {
        // An unreadable directory contributes nothing, same as compute_folder_size's walk.
        walk.stack.pop_back();
        continue;
      }
      continue;
    }
    dirent *entry = readdir(frame.dir);
    if (!entry) {
      // Directory done: remember its subtree total and roll it up into the parent.
      closedir(frame.dir);
      const std::string finished_path = frame.path;
      const std::uint64_t finished_bytes = frame.bytes;
      walk.stack.pop_back();
      browser_size_cache_[finished_path] = finished_bytes;
      fill_browser_row_size(finished_path, finished_bytes);
      if (!walk.stack.empty()) {
        walk.stack.back().bytes += finished_bytes;
      }
      continue;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    if (SCE_S_ISDIR(entry->d_stat.st_mode)) {
      const auto cached = browser_size_cache_.find(frame.path + "/" + name);
      if (cached != browser_size_cache_.end()) {
        // Already measured by an earlier walk; reuse instead of descending again.
        frame.bytes += cached->second;
      } else {
        walk.stack.push_back({frame.path + "/" + name, nullptr, 0});
      }
    } else if (SCE_S_ISREG(entry->d_stat.st_mode)) {
      frame.bytes += static_cast<std::uint64_t>(entry->d_stat.st_size);
    }
  }
}

void App::browser_toggle_selected() {
  DirectoryBrowserState &browser = directory_browser_;
  if (browser.selected >= browser.rows.size()) {
    return;
  }
  DirectoryBrowserState::Row &row = browser.rows[browser.selected];
  if (row.parent_link) {
    return;
  }
  if (row.tracked_elsewhere) {
    set_status(StatusKind::Info, "Already backed up by another entry.");
    return;
  }
  if (!row.covered_by.empty()) {
    set_status(StatusKind::Info, "Already included with \"" + row.covered_by + "\".");
    return;
  }
  const std::string full_path = browser.current_path + "/" + row.name;
  if (row.tracked_here) {
    browser_exclude_selected(full_path);
    return;
  }
  // A folder that wraps a path some other entry includes stays refused: absorbing it would
  // silently edit an entry the user is not looking at. This entry's own nested paths are handled
  // below - the parent absorbs them.
  for (const SaveRecord &save : saves_) {
    if (save.id == browser.entry_id) {
      continue;
    }
    for (const TrackedPath &extra : save.extra_paths) {
      if (path_is_inside(extra.path, full_path)) {
        set_status(StatusKind::Info, "A path inside is backed up by another entry.");
        return;
      }
    }
  }
  if (!row.size_known) {
    // The debounce usually has this already; measure now otherwise, since the size caps below need
    // it. A deliberate press gets the standard busy modal - the incremental walk exists so that
    // *browsing* never blocks, but here the user asked for this folder specifically. A walk on the
    // same folder is dropped (this measurement replaces it); one on another folder keeps going.
    ui_.draw_busy("Measuring folder", 0, -1);
    if (browser_size_walk_.active &&
        browser_size_walk_.target == full_path) {
      abort_browser_size_walk();
    }
    bool ok = false;
    const std::uint64_t bytes = compute_folder_size(full_path, &ok);
    if (ok) {
      row.size_bytes = bytes;
      row.size_known = true;
      browser_size_cache_[full_path] = bytes;
    }
  }
  constexpr std::uint64_t kMebibyte = 1024ULL * 1024ULL;
  // Saves are small. A data folder this big is almost always the game's bundled assets sitting next
  // to a tiny save, so refuse it and let the user drill into the actual save subfolder instead. The
  // limits stay in MiB so ui_.format_size (1024-based) prints them back as exactly "64 MB"/"128 MB".
  constexpr std::uint64_t kMaxBackupBytes = 128ULL * kMebibyte;
  constexpr std::uint64_t kLargeFolderWarnBytes = 64ULL * kMebibyte;
  if (row.size_known && row.size_bytes > kMaxBackupBytes) {
    // The drill-in escape hatch only makes sense for a folder; an oversized file is simply out.
    set_status(StatusKind::Error,
               "Too large to back up (over " + ui_.format_size(kMaxBackupBytes) +
                   (row.is_file ? ")." : ") - drill into the save subfolder."));
    return;
  }
  if (row.size_known && row.size_bytes > kLargeFolderWarnBytes && !browser.large_confirm_pending) {
    browser.large_confirm_pending = true;
    set_status(StatusKind::Info, std::string(row.is_file ? "Large file (" : "Large folder (") +
                                     ui_.format_size(row.size_bytes) +
                                     ") - press Square again to include it anyway.");
    return;
  }
  const std::string entry_id = browser.entry_id;
  if (entry_id.empty()) {
    // The browser is always opened from an entry; without one there is nothing to include into.
    set_status(StatusKind::Info, "No entry to include this folder in.");
    return;
  }

  // Current folders come from the record - that is the effective set, base and overrides applied.
  // An entry whose row vanished mid-visit (an orphan whose last folder was excluded) has none.
  std::vector<TrackedPath> new_paths;
  std::set<std::string> taken_prefixes;
  for (std::size_t i = 0; i < saves_.size(); ++i) {
    if (saves_[i].id == entry_id) {
      new_paths = saves_[i].extra_paths;
      break;
    }
  }
  // Including a parent absorbs this entry's own paths nested anywhere inside it - they are backed
  // up through the parent from now on, so they leave the set instead of blocking the include (the
  // reloaded rows show them as covered). Their prefixes free up with them.
  new_paths.erase(std::remove_if(new_paths.begin(), new_paths.end(),
                                 [&full_path](const TrackedPath &path) {
                                   return path_is_inside(path.path, full_path);
                                 }),
                  new_paths.end());
  for (const TrackedPath &path : new_paths) {
    taken_prefixes.insert(path.prefix);
  }
  // Allocated once, here, and stored: renaming the folder later must never repoint an existing
  // backup's restore mapping. make_extra_prefix also keeps "savedata" reserved for the entry's own
  // save folder. Re-including a base folder by hand regenerates the same prefix, which is what
  // lets the result compare equal to the base and drop the override.
  new_paths.push_back({make_extra_prefix(row.name, taken_prefixes), full_path, row.is_file});
  if (!set_entry_data_folders(entry_id, browser.entry_name, std::move(new_paths))) {
    return;
  }
  browser.changed = true;
  // Stay put: picking several folders for one app is the common case (RetroArch's savefiles and
  // savestates), and bouncing back to the grid after each one made that a chore. The grid is
  // re-sorted once, on close. No status line - the row's tag flipping is the confirmation.
  reload_browser_rows(true);
  clear_status();
}

void App::browser_exclude_selected(const std::string &full_path) {
  const std::string entry_id = directory_browser_.entry_id;
  // Current folders come from the record - the effective set, base and overrides applied - so a
  // base-provided folder excludes the same way a hand-picked one does.
  std::vector<TrackedPath> new_paths;
  const SaveRecord *entry_record = nullptr;
  for (std::size_t i = 0; i < saves_.size(); ++i) {
    if (saves_[i].id == entry_id) {
      entry_record = &saves_[i];
      new_paths = saves_[i].extra_paths;
      break;
    }
  }
  const std::size_t before = new_paths.size();
  for (std::size_t i = 0; i < new_paths.size(); ++i) {
    if (new_paths[i].path == full_path) {
      new_paths.erase(new_paths.begin() + static_cast<long>(i));
      break;
    }
  }
  if (!entry_record || new_paths.size() == before) {
    set_status(StatusKind::Info, "Not one of this entry's folders.");
    return;
  }
  // An entry whose row exists only because of its folders (an app with no savedata) loses that row
  // with its last folder - which would strand any backups it still has, so that exclude is refused
  // until they are deleted. The selection cannot move behind the picker, so backup_count() is this
  // entry's.
  const bool row_is_config_only = entry_record->path.empty();
  if (new_paths.empty() && row_is_config_only && backup_count() != 0) {
    set_status(StatusKind::Info,
               "Delete this entry's backups before excluding its last folder.");
    return;
  }
  const bool dropping_entry = new_paths.empty();
  directory_browser_.changed = true;
  if (!set_entry_data_folders(entry_id, directory_browser_.entry_name, std::move(new_paths))) {
    return;
  }
  if (dropping_entry && row_is_config_only) {
    // Nothing but the config was holding this row up (guarded above: it has no backups), and
    // apply_tracked_folders only assigns extras - it never removes a record - so the ghost row
    // would otherwise linger until the next launch. Re-including the folder in this same visit
    // recreates the entry and the row. visible_saves_ is rebuilt at once so it holds no stale
    // index into the shrunk saves_.
    for (std::size_t i = 0; i < saves_.size(); ++i) {
      if (saves_[i].id == entry_id) {
        saves_.erase(saves_.begin() + static_cast<long>(i));
        break;
      }
    }
    rebuild_visible_saves();
  }
  reload_browser_rows(true);
  // No status line - the row's tag dropping is the confirmation.
  clear_status();
}

void App::cancel_sync_all_confirmation() {
  if (sync_all_confirmation_pending_) {
    sync_all_confirmation_pending_ = false;
    // The window's checkbox edits are transient; backing out forgets them without touching the
    // remembered skip list.
    batch_deselected_.clear();
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

  // Seed the window's checkboxes from the remembered skip list - only this tab's ids, so the
  // window edits exactly what it shows. An all-skipped tab still opens (with nothing checked)
  // because the window is now also where skips are undone.
  batch_deselected_.clear();
  for (const std::size_t index : visible_saves_) {
    if (tracked_config_.skipped_ids.count(saves_[index].id) != 0) {
      batch_deselected_.insert(saves_[index].id);
    }
  }

  // Confirmation is instant: per-game work (signature check, zip, upload) is decided and done
  // during the run itself, so nothing is read twice and there is no scan phase to wait through.
  sync_all_confirmation_pending_ = true;
  sync_all_will_upload_ = drive_online;
  update_sync_all_confirm_status();
}

std::size_t App::batch_selected_count() const {
  // batch_deselected_ only ever holds visible ids, and nothing rebuilds visible_saves_ while the
  // window is open (tab switch, sort, and refresh are all suppressed), so plain subtraction holds.
  return visible_saves_.size() - batch_deselected_.size();
}

void App::update_sync_all_confirm_status() {
  set_status(StatusKind::Info,
             sync_all_confirm_message(batch_selected_count(), visible_saves_.size(),
                                      save_category_label(category_), sync_all_will_upload_));
}

void App::batch_toggle_focused() {
  if (selected_save_ >= visible_saves_.size()) {
    return;
  }
  const std::string &id = saves_[visible_saves_[selected_save_]].id;
  if (batch_deselected_.erase(id) == 0) {
    batch_deselected_.insert(id);
  }
  update_sync_all_confirm_status();
}

void App::batch_toggle_all() {
  // A held R flips the whole tab: everything checked empties the grid, anything less fills it.
  if (batch_deselected_.empty()) {
    for (const std::size_t index : visible_saves_) {
      batch_deselected_.insert(saves_[index].id);
    }
  } else {
    batch_deselected_.clear();
  }
  update_sync_all_confirm_status();
}

void App::run_sync_all() {
  sync_all_confirmation_pending_ = false;
  // Fold the window's picks into the remembered skip list - only this tab's ids, so other tabs'
  // skips survive untouched. In-memory always; the file write follows the standing rule of never
  // writing over a config that could not be read whole.
  bool picks_changed = false;
  for (const std::size_t index : visible_saves_) {
    const std::string &id = saves_[index].id;
    if (batch_deselected_.count(id) != 0) {
      picks_changed |= tracked_config_.skipped_ids.insert(id).second;
    } else {
      picks_changed |= tracked_config_.skipped_ids.erase(id) != 0;
    }
  }
  if (picks_changed && !tracked_config_load_failed_) {
    tracked_config_.modified = static_cast<long long>(std::time(nullptr));
    save_data_folders_config();
  }
  // Once, ahead of the whole run - never per game - so the picks just folded in ride this very
  // push (and a newer remote copy can still be adopted for later sweeps).
  sync_backup_settings_if_dirty();
  // Targets come from the window's own checkboxes, not from skipped_ids: if the sync above just
  // adopted a newer remote skip list, the sweep must still match exactly what the grid showed.
  std::vector<std::size_t> targets;
  targets.reserve(visible_saves_.size());
  for (const std::size_t index : visible_saves_) {
    if (batch_deselected_.count(saves_[index].id) == 0) {
      targets.push_back(index);
    }
  }
  batch_deselected_.clear();
  const std::size_t total = targets.size();
  SyncRunCounts run;
  std::size_t metadata_warnings = 0;
  bool auth_lost = false;
  // The run honors what the confirmation promised (baked when the window opened) instead of
  // re-polling reachability here: the Vita's wifi power-save can report unreachable for a moment,
  // and a fresh poll then silently downgraded the whole run to local-only - a game whose backup
  // existed but was never uploaded came back "up to date" with the upload quietly skipped. If the
  // network is genuinely gone, the first upload fails and the summary says so out loud.
  const bool drive_online = google_connected_ && sync_all_will_upload_;
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
    const std::vector<ArchiveEntryInfo> entries = compute_save_entries(save, &input.entries_ok);
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
      // The label is ignored while the batch title owns the modal; passing it anyway feeds the
      // zip's byte progress into the corner percent, which stays hidden for fast small saves
      // because endpoint reports are suppressed.
      const LocalSnapshotResult result = create_local_snapshot(save, "", "Backing up");
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
  // Started right after the bridge so every mount from here on - queued reads, the batch read,
  // backups, details - goes through the one worker thread.
  start_mount_worker();

  // Scanning storage and reading the system app database (titles, icons) blocks for a moment on a
  // full library; draw a frame first so the screen is not blank while it runs. Start at a
  // determinate 0% (total 1 is a placeholder until the scan lists the saves and reports the real
  // count) so the bar opens at 0% rather than flashing an indeterminate sweep before it fills.
  ui_.draw_busy("Loading saves", 0, 1);

  // The index loads before the scan so it can consume it: times and sfo-derived titles are
  // trusted while each save folder's fingerprint is unchanged, app-database titles while the
  // database stamp is, so a warm start skips the sdslot/sfo reads and the sqlite query entirely.
  save_index_ = read_save_index(kSaveIndexPath);
  // Version 2 folded save-times.json into the index; on an upgraded card the old file is dead
  // weight. Unconditional removal costs one failed unlink per boot once it is gone.
  std::remove(kLegacySaveTimesPath);
  load_tracked_folders();
  long long app_db_mtime = 0;
  long long app_db_size = 0;
  const bool app_db_stamped = stat_file_stamp(kSystemAppDbPath, &app_db_mtime, &app_db_size);
  const bool titles_fresh = app_db_stamped && app_db_mtime == save_index_.app_db_mtime &&
                            app_db_size == save_index_.app_db_size;

  // Scan once at startup for the foundation build. Later actions that create, restore, or delete a
  // save will refresh this list explicitly so the UI does not rescan storage every frame.
  // The index is always consulted: it spares the per-save param.sfo probes either way. The
  // stamp only decides whether the app-database query can be skipped - and the stamp read can
  // itself fail (ur0:shell is not statable by an unprivileged app even though the vendored sqlite
  // VFS can read the db), in which case the query simply runs every boot and refreshes any
  // db-derived title the scan reused, before the first frame is drawn.
  saves_ = scan_save_roots(default_save_roots(),
                           [this](std::size_t done, std::size_t total) {
                             ui_.draw_busy("Loading saves", static_cast<long long>(done),
                                           static_cast<long long>(total));
                           },
                           {}, &save_index_);

  // The app-database query (titles, icons) runs only when it can matter: the database changed
  // since the index was built, or a save appeared that the index does not cover. It has no known
  // row count, so it stays a pulse; repaint a frame every few rows so it does not look frozen.
  bool need_app_db = !titles_fresh;
  for (const SaveRecord &save : saves_) {
    if (save.platform != SavePlatform::Psp && !save.title_from_cache) {
      need_app_db = true;
      break;
    }
  }
  if (need_app_db) {
    const AppDbMetadataResult metadata_result =
        apply_app_db_metadata(&saves_, [this] { ui_.draw_busy("Loading saves", 0, -1); });
    if (!metadata_result.ok && !metadata_result.error.empty()) {
      set_status(StatusKind::Info, "Using save-folder metadata: " + metadata_result.error);
    }
  }
  // Cached times for encrypted saves must apply before the rebuild: records still carrying
  // valid times get written back instead of being reset to "never resolved" and re-mounted.
  apply_cached_save_times();
  rebuild_save_index(app_db_stamped ? app_db_mtime : 0, app_db_stamped ? app_db_size : 0);
  // Fold the extra data folders into saves_ now that the scanner, app-database pass and index
  // are done - they run only over real savedata - and before the sort/rebuild below.
  apply_tracked_folders();
  load_settings();
  // One pass for folders emptied by older versions, which left them behind. Deletes clean up as
  // they go now, so this never needs to run again. It sits under the "Loading saves" modal that is
  // already on screen - no extra stage, no extra label. Marked done even when some folder resists
  // removal: a read-only card fails identically on every boot, and retrying forever buys nothing.
  if (!cleaned_empty_backup_folders_) {
    remove_empty_backup_folders(kBackupRoot);
    cleaned_empty_backup_folders_ = true;
    save_settings();
  }
  if (save_sort_requires_all_times(sort_mode_) && !resolve_all_save_times()) {
    // Canceled the startup read (the saved sort was Last Saved); fall back to name.
    sort_mode_ = SaveSortMode::Name;
    save_settings();
  }
  // Local backups rank here already; Drive additions re-sort after the index syncs.
  apply_save_sort(&saves_, sort_mode_, newest_backup_by_folder());
  // Open on the first tab that actually has saves; with none anywhere the pick is arbitrary and
  // the grid just shows its "No saves found" state.
  category_ = SaveCategory::Homebrew;
  for (int i = 0; i < kSaveCategoryCount; ++i) {
    if (category_count(static_cast<SaveCategory>(i)) > 0) {
      category_ = static_cast<SaveCategory>(i);
      break;
    }
  }
  rebuild_visible_saves();
  queue_selected_save_time_read();
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
    if (g_transfer_budget_total > 0) {
      // One continuous bar per operation: payload requests advance it, metadata requests hold it.
      const long long shown =
          total > 0 ? std::min(g_transfer_budget_base + done, g_transfer_budget_total)
                    : g_transfer_budget_base;
      ui_.draw_busy(label, shown, g_transfer_budget_total);
      return;
    }
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
      sync_backup_settings();
    }
  }

  // Set last, not inside load_tracked_folders() itself: that call runs at the very start of
  // startup, and the app-database metadata warning, a network-startup failure, or a Drive sync
  // failure further down would each overwrite status_message_ before the first frame ever reads
  // it. Landing here, after every other startup step that can also call set_status, is what makes
  // the message actually visible.
  if (tracked_config_load_failed_) {
    set_status(StatusKind::Info,
               "Backup settings unreadable - fix or delete backup-settings.json");
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
  // L held inside the batch window flips the whole tab's checkboxes, with the same one-second
  // trigger as the other hold gestures. (R toggles individually, on the press edge - no counter.)
  int batch_l_hold_frames = 0;
  bool batch_l_hold_consumed = false;
  int network_poll_delay_frames = 0;
  while (running) {
    if (network_poll_delay_frames <= 0) {
      network_connected_ = HttpClient::network_reachable();
      network_poll_delay_frames = kFramesPerSecond;
    }
    --network_poll_delay_frames;

    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    // Buttons recovered from a frame that a queued save-time read stalled are replayed here, so a
    // tap made during that read still moves the selection instead of vanishing with the frame.
    unsigned int buttons = buttons_with_left_analog(pad) | deferred_buttons_;
    deferred_buttons_ = 0;
    if (directory_browser_.open) {
      // In the picker the right stick mirrors the left - there is no backup list for it to drive.
      if (pad.ry < kAnalogCenter - kAnalogDeadZone) {
        buttons |= SCE_CTRL_UP;
      } else if (pad.ry > kAnalogCenter + kAnalogDeadZone) {
        buttons |= SCE_CTRL_DOWN;
      }
    }
    // A finished background read is applied here, before any mode branches, so it lands whether
    // the user is on the grid, in the details screen, or mid-prompt.
    complete_async_read(false);
    const unsigned int pressed =
        apply_hold_repeat(buttons, previous_buttons, &repeat_held_buttons, &repeat_frames);

    // Details carries the overview's per-snapshot actions - transfer, backup/restore, label -
    // which all act on the inspected snapshot (the selection cannot move while this screen is
    // open, so the wrong row cannot be hit). Delete and the batch hold stay overview-only.
    if (slot_details_.open) {
      if ((pressed & cancel_button) != 0) {
        // While a prompt is pending, Circle answers it and stays on the screen; otherwise it
        // leaves, silently resetting so no primed confirmation or stale status follows back to
        // the overview.
        if (restore_confirmation_pending_ || duplicate_backup_confirmation_pending_) {
          cancel_restore_confirmation();
          cancel_duplicate_backup_confirmation();
        } else {
          slot_details_.open = false;
          clear_status();
        }
        previous_buttons = buttons;
        sceKernelDelayThread(kFrameDelayUs);
        continue;
      }
      // A pending confirmation (restore, duplicate backup) survives only an immediate repeat of
      // its own button; anything else backs out, exactly like the overview.
      if ((pressed & ~backup_button) != 0) {
        cancel_restore_confirmation();
        cancel_duplicate_backup_confirmation();
      }
      // Select transfers to the side the snapshot is missing from, as in the overview. A download
      // makes the ZIP local: reopen so its metadata, sizes, and the presence glyph reflect that
      // (recovering slot details from the fresh ZIP if the Cloud had no companion).
      if ((pressed & SCE_CTRL_SELECT) != 0) {
        const BackupRow *row_before = selected_backup_row();
        const bool was_local = row_before && row_before->has_local();
        if (row_before) {
          handle_transfer_button();
        }
        const BackupRow *row_after = selected_backup_row();
        if (row_after && row_after->has_local() && !was_local) {
          open_save_details();
        } else if (row_after) {
          slot_details_.snapshot_on_card = row_after->has_local();
          slot_details_.snapshot_in_cloud = row_after->has_remote();
          slot_details_.google_connected = google_connected_;
        }
        previous_buttons = buttons;
        sceKernelDelayThread(kFrameDelayUs);
        continue;
      }
      // On the "Savedata Paths" row the context action opens the picker. Details has to close first -
      // both the draw and input paths check it before the browser, so leaving it open would hide
      // the browser behind it - and closing the picker reopens details from where it left off.
      if ((pressed & backup_button) != 0 && data_folders_row_focused()) {
        slot_details_.open = false;
        open_directory_browser(true);
        previous_buttons = buttons;
        sceKernelDelayThread(kFrameDelayUs);
        continue;
      }
      // The context action: back up the live save, or restore the inspected snapshot (second
      // press confirms, prompted in this screen's status line).
      if ((pressed & backup_button) != 0) {
        const bool was_live_save = selected_backup_row() == nullptr;
        handle_action_button();
        const BackupRow *row_after = selected_backup_row();
        // Creating a backup moves the focus to the fresh snapshot; follow it, so the screen (and
        // its Select-to-upload nudge) shows the file that was just created instead of still
        // presenting the live save. Its companion was just written, so the reopen is instant.
        if (was_live_save && row_after != nullptr) {
          open_save_details();
        } else if (row_after != nullptr) {
          // Restoring a Cloud-only snapshot downloads it to the card first; the header glyph
          // follows the row's new presence.
          slot_details_.snapshot_on_card = row_after->has_local();
          slot_details_.snapshot_in_cloud = row_after->has_remote();
          slot_details_.google_connected = google_connected_;
        }
        previous_buttons = buttons;
        sceKernelDelayThread(kFrameDelayUs);
        continue;
      }
      // Square edits the focused snapshot's label directly - the sort tap has no meaning here, so
      // no hold needed. Reopen afterwards so the header shows the new name (metadata is cached by
      // then).
      if ((pressed & SCE_CTRL_SQUARE) != 0) {
        if (selected_backup_row()) {
          begin_label_edit();
          open_save_details();
        }
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
      // Status and confirmation state feed this screen's own status line, so the carried-over
      // actions give the same feedback they give in the overview.
      details_ui.status_message = status_message_;
      details_ui.status_kind = status_kind_;
      details_ui.restore_confirmation_pending = restore_confirmation_pending_;
      details_ui.duplicate_backup_confirmation_pending = duplicate_backup_confirmation_pending_;
      ui_.draw(details_ui);
      previous_buttons = buttons;
      sceKernelDelayThread(kFrameDelayUs);
      continue;
    }

    // The data-folder picker is its own input/rendering mode, like details: it takes over the
    // screen and the selection cannot move behind it. Confined to ux0:data, the cancel button
    // climbs back up a level at a time and only closes at the root, so an included folder can
    // never end up outside ux0.
    if (directory_browser_.open) {
      DirectoryBrowserState &browser = directory_browser_;
      const std::size_t row_count = browser.rows.size();
      // Any input but a repeat Square abandons a pending large-folder confirmation, the same way a
      // second-press confirmation clears in the overview.
      if ((pressed & ~static_cast<unsigned int>(SCE_CTRL_SQUARE)) != 0) {
        browser.large_confirm_pending = false;
      }
      if (row_count > 0 && (pressed & SCE_CTRL_UP) != 0) {
        browser.selected = move_selection(browser.selected, row_count, -1);
        schedule_browser_size_resolve();
      }
      if (row_count > 0 && (pressed & SCE_CTRL_DOWN) != 0) {
        browser.selected = move_selection(browser.selected, row_count, 1);
        schedule_browser_size_resolve();
      }
      bool rows_changed = false;
      // L/R hop across the entry's included folders, wherever they live.
      if ((pressed & SCE_CTRL_LTRIGGER) != 0) {
        browser_jump_included(-1);
        rows_changed = true;
      }
      if (!rows_changed && (pressed & SCE_CTRL_RTRIGGER) != 0) {
        browser_jump_included(1);
        rows_changed = true;
      }
      if (!rows_changed && (pressed & backup_button) != 0 && browser.selected < browser.rows.size()) {
        // Cross opens the focused folder; on the ".." row it climbs instead. A file row has
        // nothing to open - Square is its whole interaction - so the press falls through.
        if (browser.rows[browser.selected].parent_link) {
          browser_go_up();
          rows_changed = true;
        } else if (!browser.rows[browser.selected].is_file) {
          clear_status();
          browser.current_path += "/" + browser.rows[browser.selected].name;
          reload_browser_rows();
          rows_changed = true;
        }
      }
      if (!rows_changed && (pressed & cancel_button) != 0) {
        // Circle climbs one level at a time while strictly inside the folder the picker opened at
        // and closes once back there (or anywhere outside it, reachable via ".."). Higher than the
        // start is the ".." row's job.
        if (path_is_inside(browser.current_path, browser.start_path)) {
          browser_go_up();
          rows_changed = true;
        } else {
          close_directory_browser();
        }
      }
      if (directory_browser_.open && (pressed & SCE_CTRL_TRIANGLE) != 0) {
        close_directory_browser();
      }
      if (directory_browser_.open && !rows_changed && (pressed & SCE_CTRL_SQUARE) != 0) {
        browser_toggle_selected();
      }
      if (!directory_browser_.open) {
        // Closed this frame (cancel at the root, or Triangle); skip the partial browser draw and
        // let the next iteration render the overview or details with a full UiState.
        previous_buttons = buttons;
        sceKernelDelayThread(kFrameDelayUs);
        continue;
      }
      // Start sizing the focused row once the selection settles, then advance the walk one slice
      // per frame - the spinner animates while input stays live.
      if (pending_browser_size_frames_ > 0) {
        --pending_browser_size_frames_;
      } else if (pending_browser_size_frames_ == 0) {
        pending_browser_size_frames_ = -1;
        start_browser_size_walk();
      }
      advance_browser_size_walk();

      UiState browser_ui;
      browser_ui.enter_is_cross = enter_is_cross_;
      browser_ui.directory_browser = &directory_browser_;
      browser_ui.status_message = status_message_;
      browser_ui.status_kind = status_kind_;
      ui_.draw(browser_ui);
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

    // The setup prompt closes on any button, like the other prompts backing out. The consumed
    // flag swallows a confirm press so closing the prompt cannot also act on the row underneath.
    bool setup_prompt_consumed_confirm = false;
    if (google_setup_prompt_ != GoogleSetupPrompt::None && pressed != 0) {
      google_setup_prompt_ = GoogleSetupPrompt::None;
      if ((pressed & backup_button) != 0) {
        setup_prompt_consumed_confirm = true;
      }
    }

    if ((pressed & SCE_CTRL_LEFT) != 0) {
      move_selected_save(-1);
    }
    if ((pressed & SCE_CTRL_RIGHT) != 0) {
      move_selected_save(1);
    }
    if ((pressed & SCE_CTRL_UP) != 0) {
      move_selected_save_vertical(-1);
    }
    if ((pressed & SCE_CTRL_DOWN) != 0) {
      move_selected_save_vertical(1);
    }
    // Inside the batch window the shoulder buttons belong to the checkboxes, so both drop their
    // tab-switch meaning (a tab switch would tear the window down sideways): R flips the focused
    // save on the press edge, holding L a full second flips the whole tab.
    if (!sync_all_confirmation_pending_) {
      if ((pressed & SCE_CTRL_LTRIGGER) != 0) {
        move_selected_category(-1);
      }
      if ((pressed & SCE_CTRL_RTRIGGER) != 0) {
        move_selected_category(1);
      }
      batch_l_hold_frames = 0;
      batch_l_hold_consumed = false;
    } else {
      if ((pressed & SCE_CTRL_RTRIGGER) != 0) {
        batch_toggle_focused();
      }
      if ((buttons & SCE_CTRL_LTRIGGER) != 0) {
        ++batch_l_hold_frames;
        if (!batch_l_hold_consumed && batch_l_hold_frames >= kSelectHoldTriggerFrames) {
          batch_l_hold_consumed = true;
          batch_toggle_all();
        }
      } else {
        batch_l_hold_frames = 0;
        batch_l_hold_consumed = false;
      }
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
    if ((pressed & backup_button) != 0 && !setup_prompt_consumed_confirm) {
      handle_action_button();
    }
    if ((pressed & cancel_button) != 0) {
      cancel_restore_confirmation();
      cancel_delete_confirmation();
      cancel_sync_all_confirmation();
      cancel_duplicate_backup_confirmation();
      cancel_google_auth();
    }
    if (sync_all_confirmation_pending_) {
      // The window is modal for everything but browsing, the shoulder buttons, confirm and
      // cancel: details, transfers, deletes, sorting and the label editor act on rows its panel
      // covers.
      // Counters pin at zero so a button held into or across the window cannot fire as it
      // closes; Select keeps its consumed flag while held, so the hold that opened the
      // window cannot immediately reopen it after the run.
      triangle_hold_frames = 0;
      triangle_hold_consumed = false;
      square_hold_frames = 0;
      square_hold_consumed = false;
      select_hold_frames = 0;
      if ((pressed & SCE_CTRL_SELECT) != 0) {
        // A fresh Select press confirms, as an unlabeled twin of the confirm button - the finger
        // that held Select to open the window is already there. Consumed, so the press cannot
        // count toward re-arming the batch gesture as the window closes.
        select_hold_consumed = true;
        handle_action_button();
      } else if ((buttons & SCE_CTRL_SELECT) == 0) {
        select_hold_consumed = false;
      }
    } else {
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
    }

    // Queued save times feed the mount worker from here. Submitting costs the frame nothing, so
    // there is no idle gate any more: the next queued save goes out the moment the worker frees
    // up, and spinners fill in while the user keeps scrolling.
    input_idle_frames_ = buttons == 0 ? input_idle_frames_ + 1 : 0;
    if (details_open_pending_) {
      // A Triangle press is waiting on the focused save's time. That is an explicit demand, so
      // the focused save goes to the worker first (the queue's focused-first rule) and the
      // screen opens on the frame its result lands.
      const SaveRecord *focused = selected_save_record();
      if (focused && focused->save_time_requires_mount) {
        queue_selected_save_time_read();
        drain_pending_time_read();
        focused = selected_save_record();
      }
      if (!focused || !focused->save_time_requires_mount) {
        details_open_pending_ = false;
        open_save_details();
      }
    } else {
      drain_pending_time_read();
      // The write still blocks a frame, so it keeps its deep-idle gate and waits out any read
      // in flight - a landing result would just dirty the index again anyway.
      if (!mount_work_.async && input_idle_frames_ >= kSaveIndexWriteIdleFrames) {
        write_save_index_when_idle();
      }
    }

    UiState ui_state;
    ui_state.saves = &saves_;
    ui_state.visible_saves = &visible_saves_;
    ui_state.batch_deselected = &batch_deselected_;
    ui_state.batch_selected_count = batch_selected_count();
    ui_state.batch_total_count = visible_saves_.size();
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
    ui_state.sync_all_will_upload = sync_all_will_upload_;
    ui_state.duplicate_backup_confirmation_pending = duplicate_backup_confirmation_pending_;
    ui_state.enter_is_cross = enter_is_cross_;
    ui_state.slot_details = &slot_details_;
    ui_state.google_connected = google_connected_;
    ui_state.google_setup_prompt = google_setup_prompt_;
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
    } else if (!batch_l_hold_consumed && batch_l_hold_frames >= kSelectHoldTapFrames) {
      ui_state.hold_gauge_fraction = gauge_fraction(batch_l_hold_frames);
      ui_state.status_message = batch_deselected_.empty() ? "Keep holding to unselect all"
                                                          : "Keep holding to select all";
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
        sync_backup_settings();
        set_status(StatusKind::Success, "Google Drive connected.");
      }
    }

    // This keeps the placeholder loop from busy-spinning. Vita2d swaps on vblank when configured,
    // but the small delay also keeps CPU use reasonable if vblank wait is disabled by a future build.
    sceKernelDelayThread(kFrameDelayUs);
  }

  // Land the read in flight, stop the worker, then write: a clean exit keeps every resolved time
  // instead of leaving the next boot to re-mount for it.
  complete_async_read(true);
  stop_mount_worker();
  flush_save_index();
  HttpClient::network_shutdown();
  ui_.shutdown();
  sceKernelExitProcess(0);
  return 0;
}

} // namespace vsm::vita
