#include "vita/App.hpp"

#include "core/BackupArchive.hpp"
#include "core/BackupStore.hpp"
#include "core/SaveScanner.hpp"
#include "core/Selection.hpp"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <ctime>
#include <string>
#include <vector>

namespace vsm::vita {
namespace {

constexpr int kFrameDelayUs = 16 * 1000;
constexpr const char *kBackupRoot = "ux0:data/save-keeper/backups";

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

int App::run() {
  if (!ui_.initialize()) {
    return -1;
  }

  // Scan once at startup for the foundation build. Later actions that create, restore, or delete a
  // save will refresh this list explicitly so the UI does not rescan storage every frame.
  saves_ = scan_save_roots(default_save_roots());
  refresh_local_backups();

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

    ui_.draw(saves_, selected_save_, local_backups_, selected_backup_,
             restore_confirmation_pending_, status_message_);
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
