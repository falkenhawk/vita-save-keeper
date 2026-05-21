#include "vita/SaveAppDbMetadata.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <psp2/sysmodule.h>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>

extern "C" {
struct sqlite3;
int sqlite3_open_v2(const char *filename, sqlite3 **database, int flags, const char *vfs);
int sqlite3_close(sqlite3 *database);
int sqlite3_exec(sqlite3 *database, const char *sql,
                 int (*callback)(void *, int, char **, char **), void *context, char **error);
const char *sqlite3_errmsg(sqlite3 *database);
void sqlite3_free(void *value);
}

namespace vsm::vita {
namespace {

constexpr int kSqliteOk = 0;
constexpr int kSqliteOpenReadwrite = 0x00000002;
constexpr int kSqliteOpenReadonly = 0x00000001;
constexpr const char *kAppDbPath = "ur0:/shell/db/app.db";
constexpr const char *kDataRoot = "ux0:data/save-keeper";
constexpr const char *kAppDbCopyPath = "ux0:data/save-keeper/app.db";

struct AppDbTitle {
  std::string title_id;
  std::string real_id;
  std::string title;
  std::string icon_path;
};

struct AppDbContext {
  std::vector<AppDbTitle> titles;
};

std::string value_or_empty(char *value) {
  return value ? std::string(value) : std::string();
}

void normalize_title(std::string *title) {
  std::replace(title->begin(), title->end(), '\n', ' ');
}

int app_db_callback(void *context, int argc, char **argv, char **) {
  if (argc < 4) {
    return 0;
  }

  AppDbTitle title;
  title.title_id = value_or_empty(argv[0]);
  title.real_id = value_or_empty(argv[1]);
  title.title = value_or_empty(argv[2]);
  title.icon_path = value_or_empty(argv[3]);
  normalize_title(&title.title);

  if (!title.real_id.empty()) {
    static_cast<AppDbContext *>(context)->titles.push_back(std::move(title));
  }
  return 0;
}

std::unordered_map<std::string, AppDbTitle> index_by_real_id(const std::vector<AppDbTitle> &titles) {
  std::unordered_map<std::string, AppDbTitle> indexed;
  for (const AppDbTitle &title : titles) {
    indexed[title.real_id] = title;
  }
  return indexed;
}

std::unordered_map<std::string, AppDbTitle>
index_by_title_id(const std::vector<AppDbTitle> &titles) {
  std::unordered_map<std::string, AppDbTitle> indexed;
  for (const AppDbTitle &title : titles) {
    if (!title.title_id.empty()) {
      indexed[title.title_id] = title;
    }
  }
  return indexed;
}

bool ensure_directory(const char *path) {
  return mkdir(path, 0777) == 0 || errno == EEXIST;
}

bool copy_file(const char *source_path, const char *target_path) {
  FILE *source = std::fopen(source_path, "rb");
  if (!source) {
    return false;
  }

  ensure_directory(kDataRoot);
  FILE *target = std::fopen(target_path, "wb");
  if (!target) {
    std::fclose(source);
    return false;
  }

  char buffer[16 * 1024];
  bool ok = true;
  while (true) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), source);
    if (read > 0 && std::fwrite(buffer, 1, read, target) != read) {
      ok = false;
      break;
    }
    if (read < sizeof(buffer)) {
      ok = std::ferror(source) == 0;
      break;
    }
  }

  ok = std::fclose(target) == 0 && ok;
  std::fclose(source);
  return ok;
}

void apply_title_metadata(const AppDbTitle &title, SaveRecord *save) {
  save->title_id = title.title_id;
  if (!title.title.empty()) {
    save->display_name = title.title;
  }
  if (!title.icon_path.empty()) {
    save->icon_path = title.icon_path;
  }
}

std::string format_sqlite_open_error(int result_code, sqlite3 *database) {
  char message[128];
  const char *detail = database ? sqlite3_errmsg(database) : "no database handle";
  std::snprintf(message, sizeof(message), "Could not open Vita app database (%d: %s).",
                result_code, detail ? detail : "unknown error");
  return message;
}

} // namespace

AppDbMetadataResult apply_app_db_metadata(std::vector<SaveRecord> *saves) {
  AppDbMetadataResult result;
  if (!saves) {
    result.error = "No save list provided.";
    return result;
  }

  sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE);

  sqlite3 *database = nullptr;
  int open_result = sqlite3_open_v2(kAppDbPath, &database, kSqliteOpenReadonly, nullptr);
  if (open_result != kSqliteOk && database) {
    sqlite3_close(database);
    database = nullptr;
  }
  if (open_result != kSqliteOk || !database) {
    // Some Vita homebrew uses sqlite3_rw_init before opening app.db directly. That private helper
    // is not available in this build, so if SQLite refuses the shell DB path, copy the DB with
    // plain file I/O and query the local snapshot instead.
    if (!copy_file(kAppDbPath, kAppDbCopyPath)) {
      result.error = format_sqlite_open_error(open_result, database);
      if (database) {
        sqlite3_close(database);
      }
      return result;
    }
    open_result = sqlite3_open_v2(kAppDbCopyPath, &database, kSqliteOpenReadonly, nullptr);
  }
  if (open_result != kSqliteOk || !database) {
    result.error = format_sqlite_open_error(open_result, database);
    if (database) {
      sqlite3_close(database);
    }
    return result;
  }

  AppDbContext context;
  char *error = nullptr;
  // SaveCloud Vita uses this same app.db relationship: tbl_appinfo stores a game title ID and a
  // separate "realid" value, and the save directory is often named by that realid. Querying every
  // title with an icon covers homebrew saves such as BHBB00001 as well as retail PCSB/PCSE games.
  constexpr const char *kQuery =
      "select b.titleid, b.realid, c.title, c.iconpath"
      "  from (select titleid, val as realid"
      "          from tbl_appinfo"
      "         where key = 278217076) b"
      "  join tbl_appinfo_icon c"
      "    on b.titleid = c.titleid"
      "   and c.type = 0"
      " order by b.titleid";

  const int exec_result = sqlite3_exec(database, kQuery, app_db_callback, &context, &error);
  if (error) {
    sqlite3_free(error);
  }
  sqlite3_close(database);

  if (exec_result != kSqliteOk) {
    result.error = "Could not query Vita app database.";
    return result;
  }

  const std::unordered_map<std::string, AppDbTitle> by_real_id = index_by_real_id(context.titles);
  const std::unordered_map<std::string, AppDbTitle> by_title_id = index_by_title_id(context.titles);
  for (SaveRecord &save : *saves) {
    if (save.platform == SavePlatform::Psp) {
      continue;
    }

    const auto real_id = by_real_id.find(save.id);
    if (real_id != by_real_id.end()) {
      apply_title_metadata(real_id->second, &save);
      continue;
    }

    const auto title_id = by_title_id.find(save.id);
    if (title_id != by_title_id.end()) {
      apply_title_metadata(title_id->second, &save);
    }
  }

  result.ok = true;
  return result;
}

} // namespace vsm::vita
