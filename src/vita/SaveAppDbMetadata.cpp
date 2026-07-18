#include "vita/SaveAppDbMetadata.hpp"

#include <algorithm>
#include <cstdio>
#include <psp2/sysmodule.h>
#include <string>
#include <unordered_map>
#include <utility>
#include <vita_sqlite_rw.h>

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
constexpr int kSqliteOpenReadonly = 0x00000001;

struct AppDbTitle {
  std::string title_id;
  std::string real_id;
  std::string title;
  std::string icon_path;
};

struct AppDbContext {
  std::vector<AppDbTitle> titles;
  const std::function<void()> *on_progress = nullptr;
  std::size_t rows = 0;
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

  auto *ctx = static_cast<AppDbContext *>(context);
  if (!title.real_id.empty()) {
    ctx->titles.push_back(std::move(title));
  }
  // Pulse the caller every few rows so a startup animation keeps moving during the query.
  if (ctx->on_progress && (++ctx->rows % 16 == 0)) {
    (*ctx->on_progress)();
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

void apply_title_metadata(const AppDbTitle &title, SaveRecord *save) {
  save->title_id = title.title_id;
  if (!title.title.empty()) {
    save->display_name = title.title;
  }
  if (!title.icon_path.empty()) {
    save->icon_path = title.icon_path;
  }
  // The title cache keys this entry's freshness to the app-database stamp instead of the save
  // folder fingerprint.
  save->title_from_app_db = true;
}

std::string format_sqlite_open_error(int result_code, sqlite3 *database) {
  char message[128];
  const char *detail = database ? sqlite3_errmsg(database) : "no database handle";
  std::snprintf(message, sizeof(message), "Could not open Vita app database (%d: %s).",
                result_code, detail ? detail : "unknown error");
  return message;
}

} // namespace

AppDbMetadataResult apply_app_db_metadata(std::vector<SaveRecord> *saves,
                                          const std::function<void()> &on_progress) {
  AppDbMetadataResult result;
  if (!saves) {
    result.error = "No save list provided.";
    return result;
  }

  sceSysmoduleLoadModule(SCE_SYSMODULE_SQLITE);
  // Sony's SceSqlite ships without a configured allocator and with a crippled default VFS, so a
  // plain sqlite3_open_v2 fails even read-only. The vendored VitaSmith override (also used by
  // VitaShell and Apollo Save Tool) fixes both; it must run before the first open call.
  sqlite3_rw_init();

  sqlite3 *database = nullptr;
  const int open_result =
      sqlite3_open_v2(kSystemAppDbPath, &database, kSqliteOpenReadonly, nullptr);
  if (open_result != kSqliteOk || !database) {
    result.error = format_sqlite_open_error(open_result, database);
    if (database) {
      sqlite3_close(database);
    }
    return result;
  }

  AppDbContext context;
  if (on_progress) {
    context.on_progress = &on_progress;
  }
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
