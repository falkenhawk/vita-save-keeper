#include "vita/App.hpp"

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

namespace vsm::vita {
namespace {

constexpr int kFrameDelayUs = 16 * 1000;

} // namespace

int App::run() {
  if (!ui_.initialize()) {
    return -1;
  }

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  bool running = true;
  while (running) {
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);

    // START exits immediately in this foundation build. Later screens will route input through a
    // state stack so destructive actions can require explicit confirmation or hold-to-confirm.
    if ((pad.buttons & SCE_CTRL_START) != 0) {
      running = false;
    }

    ui_.draw();

    // This keeps the placeholder loop from busy-spinning. Vita2d swaps on vblank when configured,
    // but the small delay also keeps CPU use reasonable if vblank wait is disabled by a future build.
    sceKernelDelayThread(kFrameDelayUs);
  }

  ui_.shutdown();
  sceKernelExitProcess(0);
  return 0;
}

} // namespace vsm::vita
