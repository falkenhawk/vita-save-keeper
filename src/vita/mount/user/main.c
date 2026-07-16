#include "mount_bridge.h"

#include <psp2/kernel/modulemgr.h>

int saveKeeperUserMountById(SaveKeeperMountArgs *args) {
  return saveKeeperKernelMountById(args);
}

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));
int module_start(SceSize argc, const void *args) {
  (void)argc;
  (void)args;
  return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args) {
  (void)argc;
  (void)args;
  return SCE_KERNEL_STOP_SUCCESS;
}
