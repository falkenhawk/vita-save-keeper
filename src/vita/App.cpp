#include "vita/App.hpp"

#include "core/BackupArchive.hpp"
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

int App::run() {
  if (!ui_.initialize()) {
    return -1;
  }

  // Scan once at startup for the foundation build. Later actions that create, restore, or delete a
  // save will refresh this list explicitly so the UI does not rescan storage every frame.
  saves_ = scan_save_roots(default_save_roots());

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
      selected_save_ = move_selection(selected_save_, saves_.size(), -1);
    }
    if ((pressed & SCE_CTRL_RIGHT) != 0) {
      selected_save_ = move_selection(selected_save_, saves_.size(), 1);
    }
    if ((pressed & SCE_CTRL_UP) != 0) {
      selected_save_ = move_selection(selected_save_, saves_.size(), -4);
    }
    if ((pressed & SCE_CTRL_DOWN) != 0) {
      selected_save_ = move_selection(selected_save_, saves_.size(), 4);
    }
    if ((pressed & SCE_CTRL_CIRCLE) != 0) {
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
      }
    }

    ui_.draw(saves_, selected_save_, status_message_);
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
