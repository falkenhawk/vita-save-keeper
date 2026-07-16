#pragma once

typedef struct SaveKeeperMountArgs {
  int id;
  const char *process_title_id;
  const char *path;
  const char *desired_mount_point;
  const void *key;
  char *mount_point;
} SaveKeeperMountArgs;

int saveKeeperKernelMountById(SaveKeeperMountArgs *args);
