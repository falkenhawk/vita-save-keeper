/*
 * Minimal savedata mount bridge derived from VitaShell's GPLv3 mount module:
 * https://github.com/TheOfficialFloW/VitaShell
 */

#include "mount_bridge.h"

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <stdint.h>
#include <string.h>
#include <taihen.h>

int module_get_export_func(SceUID pid, const char *module_name, uint32_t library_nid,
                           uint32_t function_nid, uintptr_t *function);
int module_get_offset(SceUID pid, SceUID module_id, int segment, size_t offset,
                      uintptr_t *address);

typedef void *(*FindProcessInfo)(void *data, SceUID pid);
typedef int (*MountById)(SceUID pid, void *info, int id, const char *title_id, const char *path,
                         const char *desired_mount_point, const void *key, char *mount_point);
typedef int (*GetModuleInfo)(SceUID pid, SceUID module_id, SceKernelModuleInfo *info);

static int mount_with_app_manager(SaveKeeperMountArgs *args) {
  tai_module_info_t app_manager;
  memset(&app_manager, 0, sizeof(app_manager));
  app_manager.size = sizeof(app_manager);
  if (taiGetModuleInfoForKernel(KERNEL_PID, "SceAppMgr", &app_manager) < 0) {
    return -1;
  }

  FindProcessInfo find_process_info = NULL;
  MountById mount_by_id = NULL;
  size_t find_offset = 0;
  size_t mount_offset = 0;

  // SceAppMgr does not export this mount operation. These offsets are the firmware-specific
  // locations used by VitaShell for every retail firmware from 3.55 through 3.73.
  switch (app_manager.module_nid) {
  case 0x94CEFE4B: // 3.55 retail
  case 0xDFBC288C: // 3.57 retail
    find_offset = 0x2DE1;
    mount_offset = 0x19E15;
    break;
  case 0xDBB29DB7: // 3.60 retail
    find_offset = 0x2DE1;
    mount_offset = 0x19B51;
    break;
  case 0x1C9879D6: // 3.65 retail
    find_offset = 0x2DE1;
    mount_offset = 0x19E61;
    break;
  case 0x54E2E984: // 3.67 retail
  case 0xC3C538DE: // 3.68 retail
    find_offset = 0x2DE1;
    mount_offset = 0x19E6D;
    break;
  case 0x321E4852: // 3.69 retail
  case 0x700DA0CD: // 3.70 retail
  case 0xF7846B4E: // 3.71 retail
  case 0xA8E80BA8: // 3.72 retail
  case 0xB299D195: // 3.73 retail
    find_offset = 0x2DE9;
    mount_offset = 0x19E95;
    break;
  default:
    return -1;
  }

  if (module_get_offset(KERNEL_PID, app_manager.modid, 0, find_offset,
                        (uintptr_t *)&find_process_info) < 0 ||
      module_get_offset(KERNEL_PID, app_manager.modid, 0, mount_offset,
                        (uintptr_t *)&mount_by_id) < 0) {
    return -1;
  }

  GetModuleInfo get_module_info = NULL;
  int result = module_get_export_func(KERNEL_PID, "SceKernelModulemgr", 0xC445FA63, 0xD269F915,
                                      (uintptr_t *)&get_module_info);
  if (result < 0) {
    result = module_get_export_func(KERNEL_PID, "SceKernelModulemgr", 0x92C9FFC2, 0xDAA90093,
                                    (uintptr_t *)&get_module_info);
  }
  if (result < 0) {
    return result;
  }

  SceKernelModuleInfo module_info;
  memset(&module_info, 0, sizeof(module_info));
  module_info.size = sizeof(module_info);
  result = get_module_info(KERNEL_PID, app_manager.modid, &module_info);
  if (result < 0) {
    return result;
  }

  const SceUID process_id = ksceKernelGetProcessId();
  void *process_info =
      find_process_info((void *)((uintptr_t)module_info.segments[1].vaddr + 0x500), process_id);
  if (!process_info) {
    return -1;
  }

  char title_id[12] = {0};
  char path[256] = {0};
  char desired_mount_point[16] = {0};
  char mount_point[16] = {0};
  char key[16] = {0};
  if (args->process_title_id) {
    ksceKernelStrncpyUserToKernel(title_id, args->process_title_id, sizeof(title_id) - 1);
  }
  if (args->path) {
    ksceKernelStrncpyUserToKernel(path, args->path, sizeof(path) - 1);
  }
  if (args->desired_mount_point) {
    ksceKernelStrncpyUserToKernel(desired_mount_point, args->desired_mount_point,
                                 sizeof(desired_mount_point) - 1);
  }
  if (args->key) {
    ksceKernelMemcpyUserToKernel(key, args->key, sizeof(key));
  }

  result = mount_by_id(process_id, (void *)((uintptr_t)process_info + 0x580), args->id,
                       args->process_title_id ? title_id : NULL, args->path ? path : NULL,
                       args->desired_mount_point ? desired_mount_point : NULL,
                       args->key ? key : NULL, mount_point);
  if (args->mount_point) {
    ksceKernelStrncpyKernelToUser(args->mount_point, mount_point, sizeof(mount_point) - 1);
  }
  return result;
}

int saveKeeperKernelMountById(SaveKeeperMountArgs *args) {
  uint32_t state;
  ENTER_SYSCALL(state);

  SaveKeeperMountArgs kernel_args;
  ksceKernelMemcpyUserToKernel(&kernel_args, args, sizeof(kernel_args));
  const int result =
      ksceKernelRunWithStack(0x2000, (void *)mount_with_app_manager, &kernel_args);

  EXIT_SYSCALL(state);
  return result;
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
