/*
  Declarations for VitaSmith's PS Vita SQLite R/W VFS override (vita_sqlite_rw.c).
  Source: https://github.com/VitaSmith/libsqlite (GPLv3), vendored verbatim.

  Sony's SceSqlite module ships a crippled default VFS ("psp2") and no memory allocator
  configuration, so plain sqlite3_open_v2() fails even for read-only use. Call
  sqlite3_rw_init() once after sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE) and before
  any sqlite3_open* call. VitaShell and Apollo Save Tool use this same override to read
  ur0:shell/db/app.db.
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int sqlite3_rw_init(void);
int sqlite3_rw_exit(void);

#ifdef __cplusplus
}
#endif
