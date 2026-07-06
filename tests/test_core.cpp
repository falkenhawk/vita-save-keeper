#include "core/AppSettings.hpp"
#include "core/BackupArchive.hpp"
#include "core/BackupList.hpp"
#include "core/BackupName.hpp"
#include "core/BackupStore.hpp"
#include "core/GoogleAuth.hpp"
#include "core/GoogleConfig.hpp"
#include "core/GoogleDrive.hpp"
#include "core/GridWindow.hpp"
#include "core/PathUtil.hpp"
#include "core/SaveCategory.hpp"
#include "core/SaveScanner.hpp"
#include "core/Selection.hpp"
#include "core/SfoParser.hpp"
#include "core/SyncPlan.hpp"
#include "core/TextUtil.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect_true(bool value, const char *expression, int line) {
  if (!value) {
    std::cerr << "line " << line << ": expected true: " << expression << "\n";
    std::exit(1);
  }
}

void expect_eq(const std::string &actual, const std::string &expected, const char *expression,
               int line) {
  if (actual != expected) {
    std::cerr << "line " << line << ": expected " << expression << " to equal '" << expected
              << "', got '" << actual << "'\n";
    std::exit(1);
  }
}

void expect_eq(std::size_t actual, std::size_t expected, const char *expression, int line) {
  if (actual != expected) {
    std::cerr << "line " << line << ": expected " << expression << " to equal " << expected
              << ", got " << actual << "\n";
    std::exit(1);
  }
}

#define EXPECT_TRUE(expression) expect_true((expression), #expression, __LINE__)
#define EXPECT_EQ(actual, expected) expect_eq((actual), (expected), #actual, __LINE__)

std::uint16_t read_le16(const std::vector<unsigned char> &data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(data[offset + 1] << 8);
}

std::uint32_t read_le32(const std::vector<unsigned char> &data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

void append_le16(std::vector<unsigned char> *data, std::uint16_t value) {
  data->push_back(static_cast<unsigned char>(value & 0xff));
  data->push_back(static_cast<unsigned char>((value >> 8) & 0xff));
}

void append_le32(std::vector<unsigned char> *data, std::uint32_t value) {
  data->push_back(static_cast<unsigned char>(value & 0xff));
  data->push_back(static_cast<unsigned char>((value >> 8) & 0xff));
  data->push_back(static_cast<unsigned char>((value >> 16) & 0xff));
  data->push_back(static_cast<unsigned char>((value >> 24) & 0xff));
}

std::size_t align4(std::size_t value) {
  return (value + 3) & ~static_cast<std::size_t>(3);
}

std::vector<unsigned char>
build_sfo_with_strings(const std::vector<std::pair<std::string, std::string>> &fields) {
  std::vector<unsigned char> key_table;
  std::vector<unsigned char> data_table;
  std::vector<std::uint16_t> key_offsets;
  std::vector<std::uint32_t> data_offsets;

  for (const auto &field : fields) {
    key_offsets.push_back(static_cast<std::uint16_t>(key_table.size()));
    key_table.insert(key_table.end(), field.first.begin(), field.first.end());
    key_table.push_back('\0');

    data_offsets.push_back(static_cast<std::uint32_t>(data_table.size()));
    data_table.insert(data_table.end(), field.second.begin(), field.second.end());
    data_table.push_back('\0');
  }

  const std::uint32_t index_count = static_cast<std::uint32_t>(fields.size());
  const std::uint32_t key_table_offset = 20 + index_count * 16;
  const std::uint32_t data_table_offset =
      static_cast<std::uint32_t>(align4(key_table_offset + key_table.size()));

  std::vector<unsigned char> sfo;
  append_le32(&sfo, 0x46535000);
  append_le32(&sfo, 0x00000101);
  append_le32(&sfo, key_table_offset);
  append_le32(&sfo, data_table_offset);
  append_le32(&sfo, index_count);

  for (std::size_t i = 0; i < fields.size(); ++i) {
    append_le16(&sfo, key_offsets[i]);
    append_le16(&sfo, 0x0204);
    append_le32(&sfo, static_cast<std::uint32_t>(fields[i].second.size() + 1));
    append_le32(&sfo, static_cast<std::uint32_t>(fields[i].second.size() + 1));
    append_le32(&sfo, data_offsets[i]);
  }

  sfo.insert(sfo.end(), key_table.begin(), key_table.end());
  sfo.resize(data_table_offset, 0);
  sfo.insert(sfo.end(), data_table.begin(), data_table.end());
  return sfo;
}

void write_binary_file(const std::filesystem::path &path, const std::vector<unsigned char> &data) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<std::string> read_zip_central_directory_names(const std::filesystem::path &zip_path) {
  std::ifstream input(zip_path, std::ios::binary);
  const std::vector<unsigned char> data((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());

  std::vector<std::string> names;
  for (std::size_t offset = 0; offset + 46 <= data.size(); ++offset) {
    if (read_le32(data, offset) != 0x02014b50) {
      continue;
    }

    const std::uint16_t name_length = read_le16(data, offset + 28);
    const std::uint16_t extra_length = read_le16(data, offset + 30);
    const std::uint16_t comment_length = read_le16(data, offset + 32);
    const std::size_t name_offset = offset + 46;
    EXPECT_TRUE(name_offset + name_length <= data.size());
    names.emplace_back(reinterpret_cast<const char *>(&data[name_offset]), name_length);
    offset = name_offset + name_length + extra_length + comment_length - 1;
  }
  return names;
}

void test_timestamped_backup_name_uses_jksv_style_zip_name() {
  const vsm::BackupTimestamp timestamp{2026, 5, 21, 16, 14, 9};

  EXPECT_EQ(vsm::make_timestamped_backup_name(timestamp), "2026-05-21 16-14-09.zip");
}

void test_backup_rows_merge_local_and_remote_by_timestamp_identity() {
  // Local and remote copies of the same snapshot collapse into one row even when a label rename
  // only reached one side; remote-only and local-only snapshots keep their own rows, all sorted
  // newest first behind the "New Backup" sentinel.
  const std::vector<vsm::BackupRow> rows = vsm::build_backup_rows(
      {"2026-05-21 16-14-00.zip", "2026-05-20 22-31-00.zip", "2026-05-18 08-00-00.zip"},
      {"2026-05-21 16-14-00 before-boss.zip", "2026-05-19 09-05-00.zip"});

  EXPECT_EQ(rows.size(), static_cast<std::size_t>(5));
  EXPECT_TRUE(rows[0].new_backup);
  EXPECT_EQ(rows[0].display_name(), "New Backup");

  // Renamed local pairs with its stale-named Drive copy by the 19-char prefix.
  EXPECT_EQ(rows[1].local_name, "2026-05-21 16-14-00 before-boss.zip");
  EXPECT_EQ(rows[1].remote_name, "2026-05-21 16-14-00.zip");
  EXPECT_EQ(rows[1].display_name(), "2026-05-21 16-14-00 before-boss");

  EXPECT_TRUE(!rows[2].has_local());
  EXPECT_EQ(rows[2].remote_name, "2026-05-20 22-31-00.zip");

  EXPECT_EQ(rows[3].local_name, "2026-05-19 09-05-00.zip");
  EXPECT_TRUE(!rows[3].has_remote());

  EXPECT_TRUE(!rows[4].has_local());
  EXPECT_EQ(rows[4].remote_name, "2026-05-18 08-00-00.zip");
}

void test_backup_rows_do_not_merge_foreign_names_sharing_a_prefix() {
  // Names without a valid timestamp pattern keep their full name as identity, so two foreign
  // zips sharing 19 leading characters stay separate rows.
  const std::vector<vsm::BackupRow> rows = vsm::build_backup_rows(
      {"my-imported-backup-v1.zip"}, {"my-imported-backup-v2.zip"});

  EXPECT_EQ(rows.size(), static_cast<std::size_t>(3));
  EXPECT_TRUE(!rows[1].has_remote() || !rows[1].has_local());
  EXPECT_TRUE(!rows[2].has_remote() || !rows[2].has_local());
}

void test_backup_name_identity_label_and_rename_helpers() {
  EXPECT_TRUE(vsm::has_backup_timestamp_prefix("2026-05-21 16-14-09.zip"));
  EXPECT_TRUE(vsm::has_backup_timestamp_prefix("2026-05-21 16-14-09 before boss.zip"));
  EXPECT_TRUE(!vsm::has_backup_timestamp_prefix("2026-05-21 16-14-09x.zip"));
  EXPECT_TRUE(!vsm::has_backup_timestamp_prefix("my-imported-backup-v1.zip"));
  EXPECT_TRUE(!vsm::has_backup_timestamp_prefix("2026-05-21_16-14-09.zip"));

  EXPECT_EQ(vsm::backup_identity("2026-05-21 16-14-09 before boss.zip"), "2026-05-21 16-14-09");
  EXPECT_EQ(vsm::backup_identity("my-imported-backup-v1.zip"), "my-imported-backup-v1");

  EXPECT_EQ(vsm::backup_label("2026-05-21 16-14-09 before boss.zip"), "before boss");
  EXPECT_EQ(vsm::backup_label("2026-05-21 16-14-09 auto.zip"), "auto");
  EXPECT_EQ(vsm::backup_label("2026-05-21 16-14-09.zip"), "");

  EXPECT_EQ(vsm::backup_name_with_label("2026-05-21 16-14-09.zip", "100% save"),
            "2026-05-21 16-14-09 100% save.zip");
  // Relabeling replaces the old label; an empty label renames back to the bare timestamp.
  EXPECT_EQ(vsm::backup_name_with_label("2026-05-21 16-14-09 before boss.zip", "after boss"),
            "2026-05-21 16-14-09 after boss.zip");
  EXPECT_EQ(vsm::backup_name_with_label("2026-05-21 16-14-09 auto.zip", ""),
            "2026-05-21 16-14-09.zip");
}

void test_backup_label_sanitizer_and_auto_conflict() {
  EXPECT_EQ(vsm::sanitize_backup_label("  before   the/boss?  "), "before the_boss_");
  EXPECT_EQ(vsm::sanitize_backup_label("tab\tand\nnewline"), "tabandnewline");
  EXPECT_EQ(vsm::sanitize_backup_label("   "), "");
  // The cap counts characters, not bytes, and never splits a UTF-8 sequence.
  const std::string a32(vsm::kMaxBackupLabelLength, 'a');
  EXPECT_EQ(vsm::sanitize_backup_label(a32 + "b"), a32);  // 33rd ASCII char dropped
  // 31 ASCII + one 2-byte codepoint = 32 characters, kept whole (byte length 33 is irrelevant).
  const std::string a31(vsm::kMaxBackupLabelLength - 1, 'a');
  EXPECT_EQ(vsm::sanitize_backup_label(a31 + "\xC3\xA9"), a31 + "\xC3\xA9");
  // 31 ASCII + two 2-byte codepoints = 33 characters, the last codepoint dropped whole.
  EXPECT_EQ(vsm::sanitize_backup_label(a31 + "\xC3\xA9\xC3\xA9"), a31 + "\xC3\xA9");

  EXPECT_TRUE(vsm::backup_label_conflicts_with_auto("auto"));
  EXPECT_TRUE(vsm::backup_label_conflicts_with_auto("my auto"));
  EXPECT_TRUE(!vsm::backup_label_conflicts_with_auto("automatic"));
  EXPECT_TRUE(!vsm::backup_label_conflicts_with_auto("autosave"));
  EXPECT_TRUE(!vsm::backup_label_conflicts_with_auto(""));
}

void test_path_component_normalization_replaces_unsafe_characters() {
  EXPECT_EQ(vsm::normalize_path_component("PCSE00120: Persona/4? Golden"),
            "PCSE00120_ Persona_4_ Golden");
  EXPECT_EQ(vsm::normalize_path_component("  ux0:\\bad*name\"  "), "ux0__bad_name_");
}

void test_sfo_parser_reads_title_strings() {
  const std::vector<unsigned char> data = build_sfo_with_strings({
      {"SAVEDATA_TITLE", "Clear Data"},
      {"TITLE", "Persona 4 Golden"},
  });

  const vsm::SfoMetadata metadata = vsm::parse_sfo_metadata(data);

  EXPECT_TRUE(metadata.ok);
  EXPECT_EQ(metadata.strings.at("TITLE"), "Persona 4 Golden");
  EXPECT_EQ(vsm::title_from_sfo_metadata(metadata), "Persona 4 Golden");
}

void test_save_scanner_lists_direct_child_save_directories() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-scanner-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "vita" / "PCSE00120");
  std::filesystem::create_directories(base / "vita" / "PCSE99999");
  std::filesystem::create_directories(base / "vita" / "PCSE00120" / "sce_sys");
  std::filesystem::create_directories(base / "psp" / "ULUS12345");
  write_binary_file(base / "vita" / "PCSE00120" / "sce_sys" / "param.sfo",
                    build_sfo_with_strings({{"TITLE", "Persona 4 Golden"},
                                            {"TITLE_ID", "PCSE00120"}}));
  std::ofstream(base / "vita" / "PCSE00120" / "sce_sys" / "icon0.png", std::ios::binary)
      << "png";
  write_binary_file(base / "psp" / "ULUS12345" / "PARAM.SFO",
                    build_sfo_with_strings({{"TITLE", "PSP Save"}}));
  std::ofstream(base / "psp" / "ULUS12345" / "ICON0.PNG", std::ios::binary) << "png";

  FILE *ignored_file = std::fopen((base / "vita" / "not-a-save.txt").string().c_str(), "w");
  EXPECT_TRUE(ignored_file != nullptr);
  std::fclose(ignored_file);

  const std::vector<vsm::SaveRecord> saves = vsm::scan_save_roots({
      {vsm::SavePlatform::Vita, (base / "vita").string()},
      {vsm::SavePlatform::Psp, (base / "psp").string()},
      {vsm::SavePlatform::GameCard, (base / "missing").string()},
  });

  EXPECT_EQ(saves.size(), static_cast<std::size_t>(3));
  EXPECT_TRUE(saves[0].platform == vsm::SavePlatform::Vita);
  EXPECT_EQ(saves[0].id, "PCSE00120");
  EXPECT_EQ(saves[0].display_name, "Persona 4 Golden");
  EXPECT_EQ(saves[0].title_id, "PCSE00120");
  EXPECT_EQ(saves[0].path, (base / "vita" / "PCSE00120").string());
  EXPECT_EQ(saves[0].icon_path, (base / "vita" / "PCSE00120" / "sce_sys" / "icon0.png").string());
  EXPECT_EQ(saves[1].id, "PCSE99999");
  EXPECT_EQ(saves[1].display_name, "PCSE99999");
  EXPECT_TRUE(saves[2].platform == vsm::SavePlatform::Psp);
  EXPECT_EQ(saves[2].id, "ULUS12345");
  EXPECT_EQ(saves[2].display_name, "PSP Save");
  EXPECT_EQ(saves[2].icon_path, (base / "psp" / "ULUS12345" / "ICON0.PNG").string());

  std::filesystem::remove_all(base);
}

void test_selection_wraps_and_handles_empty_lists() {
  EXPECT_EQ(vsm::move_selection(0, 3, -1), static_cast<std::size_t>(2));
  EXPECT_EQ(vsm::move_selection(2, 3, 1), static_cast<std::size_t>(0));
  EXPECT_EQ(vsm::move_selection(1, 3, 1), static_cast<std::size_t>(2));
  EXPECT_EQ(vsm::move_selection(12, 0, -1), static_cast<std::size_t>(0));
}

void test_grid_window_scrolls_only_when_selection_leaves_view() {
  EXPECT_EQ(vsm::grid_window_top_row(0, 12, 90, 4, 3), static_cast<std::size_t>(1));
  EXPECT_EQ(vsm::grid_window_top_row(1, 10, 90, 4, 3), static_cast<std::size_t>(1));
  EXPECT_EQ(vsm::grid_window_top_row(1, 3, 90, 4, 3), static_cast<std::size_t>(0));
  EXPECT_EQ(vsm::grid_window_top_row(99, 89, 90, 4, 3), static_cast<std::size_t>(20));
}

void test_backup_archive_creates_timestamped_zip_snapshot() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-archive-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "source" / "sce_sys");
  std::filesystem::create_directories(base / "backups");
  {
    std::ofstream(base / "source" / "data.bin", std::ios::binary) << "save-data";
    std::ofstream(base / "source" / "sce_sys" / "icon0.png", std::ios::binary) << "icon-data";
  }

  const vsm::BackupResult result = vsm::create_backup_archive({
      (base / "source").string(),
      (base / "backups").string(),
      "PCSE00120: Persona/4",
      {2026, 5, 21, 16, 14, 9},
  });

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.archive_path,
            (base / "backups" / "PCSE00120_ Persona_4" / "2026-05-21 16-14-09.zip").string());
  EXPECT_TRUE(std::filesystem::exists(result.archive_path));

  const std::vector<std::string> names = read_zip_central_directory_names(result.archive_path);
  EXPECT_EQ(names.size(), static_cast<std::size_t>(2));
  EXPECT_EQ(names[0], "data.bin");
  EXPECT_EQ(names[1], "sce_sys/icon0.png");

  std::filesystem::remove_all(base);
}

void test_backup_archive_restores_snapshot_and_removes_stale_files() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-restore-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "source" / "sce_sys");
  std::filesystem::create_directories(base / "backups");
  {
    std::ofstream(base / "source" / "data.bin", std::ios::binary) << "save-data";
    std::ofstream(base / "source" / "sce_sys" / "icon0.png", std::ios::binary) << "icon-data";
  }

  const vsm::BackupResult backup = vsm::create_backup_archive({
      (base / "source").string(),
      (base / "backups").string(),
      "PCSE00120",
      {2026, 5, 21, 16, 14},
  });
  EXPECT_TRUE(backup.ok);

  std::filesystem::remove_all(base / "source");
  std::filesystem::create_directories(base / "source" / "sce_sys");
  {
    std::ofstream(base / "source" / "data.bin", std::ios::binary) << "stale-data";
    std::ofstream(base / "source" / "extra.bin", std::ios::binary) << "should-go-away";
    std::ofstream(base / "source" / "sce_sys" / "icon0.png", std::ios::binary) << "stale-icon";
  }

  const vsm::RestoreResult restore = vsm::restore_backup_archive({
      backup.archive_path,
      (base / "source").string(),
  });

  EXPECT_TRUE(restore.ok);
  EXPECT_TRUE(!std::filesystem::exists(base / "source" / "extra.bin"));
  EXPECT_TRUE(std::filesystem::exists(base / "source" / "data.bin"));
  EXPECT_TRUE(std::filesystem::exists(base / "source" / "sce_sys" / "icon0.png"));

  std::ifstream data_file(base / "source" / "data.bin", std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(data_file)),
                         std::istreambuf_iterator<char>());
  EXPECT_EQ(data, "save-data");

  std::ifstream icon_file(base / "source" / "sce_sys" / "icon0.png", std::ios::binary);
  const std::string icon((std::istreambuf_iterator<char>(icon_file)),
                         std::istreambuf_iterator<char>());
  EXPECT_EQ(icon, "icon-data");

  std::filesystem::remove_all(base);
}

void test_backup_archive_missing_file_does_not_clear_destination() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-missing-restore-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "source");
  std::ofstream(base / "source" / "data.bin", std::ios::binary) << "keep-me";

  const vsm::RestoreResult restore = vsm::restore_backup_archive({
      (base / "missing.zip").string(),
      (base / "source").string(),
  });

  EXPECT_TRUE(!restore.ok);
  EXPECT_TRUE(std::filesystem::exists(base / "source" / "data.bin"));

  std::ifstream data_file(base / "source" / "data.bin", std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(data_file)),
                         std::istreambuf_iterator<char>());
  EXPECT_EQ(data, "keep-me");

  std::filesystem::remove_all(base);
}

void test_backup_store_lists_local_zip_backups_newest_first() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-backup-store-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "PCSE00120_ Persona_4");
  {
    std::ofstream(base / "PCSE00120_ Persona_4" / "2026-05-20 22-31.zip") << "old";
    std::ofstream(base / "PCSE00120_ Persona_4" / "2026-05-21 16-14.zip") << "new";
    std::ofstream(base / "PCSE00120_ Persona_4" / "not-a-backup.txt") << "ignore";
  }

  const std::vector<std::string> backups =
      vsm::scan_local_backup_names(base.string(), "PCSE00120: Persona/4");

  EXPECT_EQ(backups.size(), static_cast<std::size_t>(2));
  EXPECT_EQ(backups[0], "2026-05-21 16-14.zip");
  EXPECT_EQ(backups[1], "2026-05-20 22-31.zip");
  EXPECT_TRUE(vsm::scan_local_backup_names(base.string(), "missing-save").empty());

  std::filesystem::remove_all(base);
}

void test_backup_store_builds_normalized_archive_path() {
  EXPECT_EQ(vsm::local_backup_archive_path("/backups", "PCSE00120: Persona/4",
                                           "2026-05-21 16-14.zip"),
            "/backups/PCSE00120_ Persona_4/2026-05-21 16-14.zip");
}

void test_google_auth_builds_device_code_request_body() {
  EXPECT_EQ(vsm::build_device_code_request_body("client id"),
            "client_id=client%20id&scope=https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fdrive.file");
}

void test_google_auth_builds_token_poll_request_body() {
  EXPECT_EQ(vsm::build_device_token_request_body("client id", "client secret", "device/code"),
            "client_id=client%20id&client_secret=client%20secret&device_code=device%2Fcode&"
            "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code");
}

void test_google_auth_builds_refresh_request_body() {
  EXPECT_EQ(vsm::build_refresh_token_request_body("client id", "client secret", "refresh/token"),
            "client_id=client%20id&client_secret=client%20secret&refresh_token=refresh%2Ftoken&"
            "grant_type=refresh_token");
}

void test_google_drive_root_folder_name_is_psv_saves() {
  EXPECT_EQ(std::string(vsm::kGoogleDriveRootFolderName), "PSV Saves");
}

void test_google_auth_parses_device_code_success_response() {
  const vsm::DeviceCodeResponse response = vsm::parse_device_code_response(
      "{\"device_code\":\"4/device\",\"user_code\":\"GQVQ-JKEC\","
      "\"verification_url\":\"https://www.google.com/device\",\"expires_in\":1800,"
      "\"interval\":5}");

  EXPECT_TRUE(response.ok);
  EXPECT_EQ(response.device_code, "4/device");
  EXPECT_EQ(response.user_code, "GQVQ-JKEC");
  EXPECT_EQ(response.verification_url, "https://www.google.com/device");
  EXPECT_EQ(response.expires_in, 1800);
  EXPECT_EQ(response.interval, 5);
}

void test_google_auth_parses_token_pending_error() {
  const vsm::TokenResponse response = vsm::parse_token_response(
      "{\"error\":\"authorization_pending\",\"error_description\":\"Precondition Required\"}");

  EXPECT_TRUE(!response.ok);
  EXPECT_EQ(response.error, "authorization_pending");
  EXPECT_EQ(response.error_description, "Precondition Required");
}

void test_google_auth_parses_token_success_response() {
  const vsm::TokenResponse response = vsm::parse_token_response(
      "{\"access_token\":\"access\",\"expires_in\":3920,\"scope\":\"https://www.googleapis.com/"
      "auth/drive.file\",\"token_type\":\"Bearer\",\"refresh_token\":\"refresh\"}");

  EXPECT_TRUE(response.ok);
  EXPECT_EQ(response.access_token, "access");
  EXPECT_EQ(response.refresh_token, "refresh");
  EXPECT_EQ(response.token_type, "Bearer");
  EXPECT_EQ(response.expires_in, 3920);
}

void test_google_config_parses_downloaded_client_json() {
  const vsm::GoogleClientCredentials credentials = vsm::parse_google_client_credentials(
      "{\"installed\":{\"client_id\":\"client-id.apps.googleusercontent.com\","
      "\"client_secret\":\"client-secret\"}}");

  EXPECT_TRUE(credentials.ok);
  EXPECT_EQ(credentials.client_id, "client-id.apps.googleusercontent.com");
  EXPECT_EQ(credentials.client_secret, "client-secret");
}

void test_embedded_google_client_credentials_default_empty() {
  const vsm::GoogleClientCredentials credentials = vsm::embedded_google_client_credentials();

  EXPECT_TRUE(!credentials.ok);
}

void test_google_config_serializes_and_parses_token_cache() {
  vsm::GoogleTokenCache original;
  original.access_token = "access";
  original.refresh_token = "refresh";
  original.token_type = "Bearer";
  original.expires_at_epoch_seconds = 123456;

  const std::string json = vsm::serialize_google_token_cache(original);
  const vsm::GoogleTokenCache cache = vsm::parse_google_token_cache(json);

  EXPECT_TRUE(cache.ok);
  EXPECT_EQ(cache.access_token, "access");
  EXPECT_EQ(cache.refresh_token, "refresh");
  EXPECT_EQ(cache.token_type, "Bearer");
  EXPECT_EQ(cache.expires_at_epoch_seconds, static_cast<long long>(123456));
}

void test_google_auth_builds_verification_qr_url_with_prefilled_code() {
  EXPECT_EQ(vsm::build_device_verification_qr_url("https://www.google.com/device", "ABCD-EFGH"),
            "https://www.google.com/device?user_code=ABCD-EFGH");
  EXPECT_EQ(vsm::build_device_verification_qr_url("https://example.com/device?x=1", "A B"),
            "https://example.com/device?x=1&user_code=A%20B");
  EXPECT_EQ(vsm::build_device_verification_qr_url("", "ABCD-EFGH"), "");
  EXPECT_EQ(vsm::build_device_verification_qr_url("https://www.google.com/device", ""),
            "https://www.google.com/device");
}

void test_google_drive_builds_folder_metadata_json() {
  EXPECT_EQ(vsm::build_drive_folder_metadata_json("PSV Saves", "root"),
            "{\"name\":\"PSV Saves\",\"mimeType\":\"application/vnd.google-apps.folder\","
            "\"parents\":[\"root\"]}");
}

void test_google_drive_builds_find_folder_query() {
  EXPECT_EQ(vsm::build_drive_find_folder_query("PSV Saves", "root"),
            "q=mimeType%3D%27application%2Fvnd.google-apps.folder%27%20and%20name%3D%27PSV%"
            "20Saves%27%20and%20%27root%27%20in%20parents%20and%20trashed%3Dfalse&fields=files%"
            "28id%2Cname%29");
}

void test_google_drive_parses_first_file_id() {
  const vsm::DriveFileList files =
      vsm::parse_drive_file_list("{\"files\":[{\"id\":\"folder-id\",\"name\":\"PSV Saves\"}]}");

  EXPECT_TRUE(files.ok);
  EXPECT_EQ(files.files.size(), static_cast<std::size_t>(1));
  EXPECT_EQ(files.files[0].id, "folder-id");
  EXPECT_EQ(files.files[0].name, "PSV Saves");
}

void test_google_drive_builds_upload_metadata_json() {
  EXPECT_EQ(vsm::build_drive_upload_metadata_json("2026-05-21 16-14.zip", "folder-id"),
            "{\"name\":\"2026-05-21 16-14.zip\",\"parents\":[\"folder-id\"]}");
}

void test_google_drive_builds_list_children_query() {
  EXPECT_EQ(vsm::build_drive_list_children_query("folder-id"),
            "q=%27folder-id%27%20in%20parents%20and%20trashed%3Dfalse&fields=files%28id%2Cname%"
            "29");
}

void test_google_drive_builds_paged_index_queries() {
  EXPECT_EQ(vsm::build_drive_list_all_folders_query(""),
            "q=mimeType%3D%27application%2Fvnd.google-apps.folder%27%20and%20trashed%3Dfalse"
            "&fields=nextPageToken%2Cfiles%28id%2Cname%2Cparents%29&pageSize=1000");
  EXPECT_EQ(vsm::build_drive_list_all_files_query("token-1"),
            "q=mimeType%21%3D%27application%2Fvnd.google-apps.folder%27%20and%20trashed%3Dfalse"
            "&fields=nextPageToken%2Cfiles%28id%2Cname%2Cparents%29&pageSize=1000"
            "&pageToken=token-1");
}

void test_google_drive_parses_parents_and_page_token() {
  // Shape taken from a live Drive v3 response: pretty-printed, and "parents" comes BEFORE
  // "id"/"name" inside each object. The parser must not depend on field order.
  const vsm::DriveFileList files = vsm::parse_drive_file_list(
      "{\n \"nextPageToken\": \"tok-2\",\n \"files\": [\n"
      "  {\n   \"parents\": [\n    \"folder-a\"\n   ],\n"
      "   \"id\": \"zip-1\",\n   \"name\": \"2026-07-03 23-19.zip\"\n  },\n"
      "  {\n   \"id\": \"orphan\",\n   \"name\": \"loose.zip\"\n  },\n"
      "  {\n   \"id\": \"zip-2\",\n   \"name\": \"2026-07-01 10-00.zip\",\n"
      "   \"parents\": [\n    \"folder-b\"\n   ]\n  }\n ]\n}\n");

  EXPECT_TRUE(files.ok);
  EXPECT_EQ(files.next_page_token, "tok-2");
  EXPECT_EQ(files.files.size(), static_cast<std::size_t>(3));
  EXPECT_EQ(files.files[0].id, "zip-1");
  EXPECT_EQ(files.files[0].name, "2026-07-03 23-19.zip");
  EXPECT_EQ(files.files[0].parent_id, "folder-a");
  EXPECT_EQ(files.files[1].parent_id, "");
  EXPECT_EQ(files.files[2].id, "zip-2");
  EXPECT_EQ(files.files[2].parent_id, "folder-b");
}

void test_google_drive_parses_single_object_upload_response() {
  const vsm::DriveFileList uploaded = vsm::parse_drive_file_list(
      "{\n \"id\": \"file-9\",\n \"name\": \"2026-07-04 01-21-32.zip\"\n}\n");

  EXPECT_TRUE(uploaded.ok);
  EXPECT_EQ(uploaded.files.size(), static_cast<std::size_t>(1));
  EXPECT_EQ(uploaded.files[0].id, "file-9");
  EXPECT_EQ(uploaded.files[0].name, "2026-07-04 01-21-32.zip");

  const vsm::DriveFileList empty = vsm::parse_drive_file_list("{\n \"files\": []\n}\n");
  EXPECT_TRUE(empty.ok);
  EXPECT_EQ(empty.files.size(), static_cast<std::size_t>(0));
}

void test_save_category_classification() {
  vsm::SaveRecord retail;
  retail.platform = vsm::SavePlatform::Vita;
  retail.id = "PCSB00411";
  EXPECT_TRUE(vsm::classify_save(retail) == vsm::SaveCategory::VitaGame);

  vsm::SaveRecord homebrew;
  homebrew.platform = vsm::SavePlatform::Vita;
  homebrew.id = "VITADBDLD";
  EXPECT_TRUE(vsm::classify_save(homebrew) == vsm::SaveCategory::Homebrew);

  vsm::SaveRecord short_id;
  short_id.platform = vsm::SavePlatform::Vita;
  short_id.id = "PCSB1";
  EXPECT_TRUE(vsm::classify_save(short_id) == vsm::SaveCategory::Homebrew);

  vsm::SaveRecord title_id_wins;
  title_id_wins.platform = vsm::SavePlatform::Vita;
  title_id_wins.id = "SHAREDSAV";
  title_id_wins.title_id = "PCSE00099";
  EXPECT_TRUE(vsm::classify_save(title_id_wins) == vsm::SaveCategory::VitaGame);

  vsm::SaveRecord card;
  card.platform = vsm::SavePlatform::GameCard;
  card.id = "WHATEVER1";
  EXPECT_TRUE(vsm::classify_save(card) == vsm::SaveCategory::VitaGame);

  vsm::SaveRecord psp;
  psp.platform = vsm::SavePlatform::Psp;
  psp.id = "UCES00002000";
  EXPECT_TRUE(vsm::classify_save(psp) == vsm::SaveCategory::Psp);
}

void test_save_sort_modes_order_saves() {
  std::vector<vsm::SaveRecord> saves(3);
  saves[0].id = "PCSB00001";
  saves[0].display_name = "Alpha";
  saves[0].saved_at_epoch = 100;
  saves[1].id = "PCSB00002";
  saves[1].display_name = "Bravo";
  saves[1].saved_at_epoch = 300;
  saves[2].id = "PCSB00003";
  saves[2].display_name = "Charlie";
  saves[2].saved_at_epoch = 200;

  vsm::apply_save_sort(&saves, vsm::SaveSortMode::LastSaved, {});
  EXPECT_EQ(saves[0].display_name, "Bravo");
  EXPECT_EQ(saves[1].display_name, "Charlie");
  EXPECT_EQ(saves[2].display_name, "Alpha");

  // Only Alpha and Charlie exist on Drive; Charlie's backup is newer, Bravo sinks to the end.
  const std::map<std::string, std::string> newest = {
      {"PCSB00001", "2026-07-01 10-00-00.zip"},
      {"PCSB00003", "2026-07-04 09-30-00.zip"},
  };
  vsm::apply_save_sort(&saves, vsm::SaveSortMode::LastSynced, newest);
  EXPECT_EQ(saves[0].display_name, "Charlie");
  EXPECT_EQ(saves[1].display_name, "Alpha");
  EXPECT_EQ(saves[2].display_name, "Bravo");

  vsm::apply_save_sort(&saves, vsm::SaveSortMode::Name, {});
  EXPECT_EQ(saves[0].display_name, "Alpha");
}

void test_app_settings_roundtrip_and_unknown_keys() {
  vsm::AppSettings settings;
  settings.sort_mode = vsm::SaveSortMode::LastSynced;
  EXPECT_EQ(vsm::serialize_app_settings(settings), "sort=synced\n");

  const vsm::AppSettings parsed =
      vsm::parse_app_settings("future_key=whatever\r\nsort=saved\n");
  EXPECT_TRUE(parsed.sort_mode == vsm::SaveSortMode::LastSaved);

  const vsm::AppSettings defaults = vsm::parse_app_settings("");
  EXPECT_TRUE(defaults.sort_mode == vsm::SaveSortMode::Name);
}

void test_auto_backup_suffix_display_and_content_matching() {
  vsm::BackupRow plain;
  plain.local_name = "2026-07-05 10-00-00.zip";
  EXPECT_EQ(plain.display_name(), "2026-07-05 10-00-00");
  vsm::BackupRow automatic;
  automatic.local_name = "2026-07-05 10-00-00 auto.zip";
  EXPECT_EQ(automatic.display_name(), "[AUTO] 2026-07-05 10-00-00");
  // The auto marker travels with the name, so a cloud-only auto snapshot renders the same way.
  vsm::BackupRow remote_auto;
  remote_auto.remote_name = "2026-07-05 10-00-00 auto.zip";
  EXPECT_EQ(remote_auto.display_name(), "[AUTO] 2026-07-05 10-00-00");

  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-auto-backup-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "source" / "sce_sys");
  {
    std::ofstream(base / "source" / "data.bin", std::ios::binary) << "save-data";
    std::ofstream(base / "source" / "sce_sys" / "icon0.png", std::ios::binary) << "icon-data";
  }

  vsm::BackupRequest request;
  request.source_path = (base / "source").string();
  request.backup_root = (base / "backups").string();
  request.save_id = "PCSB00001";
  request.timestamp = {2026, 7, 5, 10, 0, 0};
  request.name_suffix = " auto";
  const vsm::BackupResult result = vsm::create_backup_archive(request);
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.archive_path.find(" auto.zip") != std::string::npos);

  bool ok = false;
  const std::vector<vsm::ArchiveEntryInfo> entries =
      vsm::compute_folder_entries((base / "source").string(), &ok);
  EXPECT_TRUE(ok);
  EXPECT_EQ(entries.size(), static_cast<std::size_t>(2));
  EXPECT_TRUE(vsm::entries_match_backup_archive(entries, result.archive_path));

  // Changing a file's content must break the match even though the file list is identical.
  std::ofstream(base / "source" / "data.bin", std::ios::binary) << "different!";
  bool changed_ok = false;
  const std::vector<vsm::ArchiveEntryInfo> changed =
      vsm::compute_folder_entries((base / "source").string(), &changed_ok);
  EXPECT_TRUE(changed_ok);
  EXPECT_TRUE(!vsm::entries_match_backup_archive(changed, result.archive_path));

  // An extra file must break the match too.
  std::ofstream(base / "source" / "data.bin", std::ios::binary) << "save-data";
  std::ofstream(base / "source" / "extra.bin", std::ios::binary) << "x";
  bool extra_ok = false;
  const std::vector<vsm::ArchiveEntryInfo> extra =
      vsm::compute_folder_entries((base / "source").string(), &extra_ok);
  EXPECT_TRUE(extra_ok);
  EXPECT_TRUE(!vsm::entries_match_backup_archive(extra, result.archive_path));

  std::filesystem::remove_all(base);
}

void test_utf8_truncation_and_system_font_detection() {
  // ASCII: byte-based truncation behaves as before.
  EXPECT_EQ(vsm::truncate_utf8_label("The Walking Dead", 30), "The Walking Dead");
  EXPECT_EQ(vsm::truncate_utf8_label("abcdefghij", 8), "abcde...");

  // Japanese: each kana/kanji is 3 bytes; the cut must land on a sequence boundary.
  const std::string taiko = "\xE5\xA4\xAA\xE9\xBC\x93\xE3\x81\xAE\xE9\x81\x94\xE4\xBA\xBA";
  const std::string truncated = vsm::truncate_utf8_label(taiko, 10);
  EXPECT_EQ(truncated, std::string("\xE5\xA4\xAA\xE9\xBC\x93") + "...");

  EXPECT_TRUE(vsm::needs_system_font(taiko));
  EXPECT_TRUE(!vsm::needs_system_font("Robotics; Notes ELITE"));
  EXPECT_TRUE(!vsm::needs_system_font("Cafe \xC3\xA9" "clair"));
}

void test_saves_sort_by_display_name_case_insensitive() {
  std::vector<vsm::SaveRecord> saves(3);
  saves[0].id = "PCSB00002";
  saves[0].display_name = "the Walking Dead";
  saves[1].id = "PCSB00001";
  saves[1].display_name = "Axiom Verge";
  saves[2].id = "NONAME001";
  saves[2].display_name = "";

  vsm::sort_saves_by_display_name(&saves);

  EXPECT_EQ(saves[0].display_name, "Axiom Verge");
  EXPECT_EQ(saves[1].id, "NONAME001");
  EXPECT_EQ(saves[2].display_name, "the Walking Dead");
}

void test_sync_plan_decides_backup_and_upload_per_game() {
  // Changed content while connected: fresh backup, and the fresh archive is the upload (its name
  // is only known after creation, so upload_existing stays empty).
  {
    const vsm::SyncItemPlan plan =
        vsm::plan_sync_item({true, false, false, "2026-05-21 16-14.zip", true, true});
    EXPECT_TRUE(!plan.unreadable);
    EXPECT_TRUE(plan.create_backup);
    EXPECT_TRUE(plan.will_upload);
    EXPECT_EQ(plan.upload_existing, "");
  }

  // First-ever backup of a game: no archives yet, content counts as changed.
  {
    const vsm::SyncItemPlan plan = vsm::plan_sync_item({true, false, false, "", false, true});
    EXPECT_TRUE(plan.create_backup);
    EXPECT_TRUE(plan.will_upload);
  }

  // Changed content while offline: backup still happens, nothing uploads.
  {
    const vsm::SyncItemPlan plan =
        vsm::plan_sync_item({true, false, false, "2026-05-21 16-14.zip", false, false});
    EXPECT_TRUE(plan.create_backup);
    EXPECT_TRUE(!plan.will_upload);
    EXPECT_EQ(plan.upload_existing, "");
  }

  // Unchanged content whose newest archive is missing from Drive: upload the existing archive.
  {
    const vsm::SyncItemPlan plan =
        vsm::plan_sync_item({true, false, true, "2026-05-21 16-14.zip", false, true});
    EXPECT_TRUE(!plan.create_backup);
    EXPECT_TRUE(plan.will_upload);
    EXPECT_EQ(plan.upload_existing, "2026-05-21 16-14.zip");
  }

  // Unchanged and already on Drive: nothing to do.
  {
    const vsm::SyncItemPlan plan =
        vsm::plan_sync_item({true, false, true, "2026-05-21 16-14.zip", true, true});
    EXPECT_TRUE(!plan.create_backup);
    EXPECT_TRUE(!plan.will_upload);
    EXPECT_EQ(plan.upload_existing, "");
  }

  // Unchanged while offline: no upload even though Drive lacks the archive.
  {
    const vsm::SyncItemPlan plan =
        vsm::plan_sync_item({true, false, true, "2026-05-21 16-14.zip", false, false});
    EXPECT_TRUE(!plan.will_upload);
    EXPECT_EQ(plan.upload_existing, "");
  }

  // Unreadable folder: skipped entirely, even when an existing archive could upload.
  {
    const vsm::SyncItemPlan plan =
        vsm::plan_sync_item({false, false, false, "2026-05-21 16-14.zip", false, true});
    EXPECT_TRUE(plan.unreadable);
    EXPECT_TRUE(!plan.create_backup);
    EXPECT_TRUE(!plan.will_upload);
    EXPECT_EQ(plan.upload_existing, "");
  }

  // Empty folder with no archives: nothing to back up or upload.
  {
    const vsm::SyncItemPlan plan = vsm::plan_sync_item({true, true, false, "", false, true});
    EXPECT_TRUE(!plan.unreadable);
    EXPECT_TRUE(!plan.create_backup);
    EXPECT_TRUE(!plan.will_upload);
  }

  // Empty folder but an old archive exists and Drive lacks it: the archive still uploads.
  {
    const vsm::SyncItemPlan plan =
        vsm::plan_sync_item({true, true, false, "2026-05-20 22-31.zip", false, true});
    EXPECT_TRUE(!plan.create_backup);
    EXPECT_TRUE(plan.will_upload);
    EXPECT_EQ(plan.upload_existing, "2026-05-20 22-31.zip");
  }
}

void test_sync_all_confirm_message_states_scope() {
  EXPECT_EQ(vsm::sync_all_confirm_message(73, "Vita", true),
            "Backup & upload all 73 Vita saves?");
  // Offline is already indicated by the header and by the missing "& upload" in the verb; the
  // prompt does not repeat it.
  EXPECT_EQ(vsm::sync_all_confirm_message(4, "PSP", false), "Backup all 4 PSP saves?");
  EXPECT_EQ(vsm::sync_all_confirm_message(1, "Homebrew", true),
            "Backup & upload 1 Homebrew save?");
}

void test_sync_run_summary_reports_results_and_cancellation() {
  EXPECT_EQ(vsm::sync_run_summary({3, 8, 0, 0, 0}), "Backed up 3, uploaded 8.");
  EXPECT_EQ(vsm::sync_run_summary({3, 0, 0, 0, 0}), "Backed up 3.");
  EXPECT_EQ(vsm::sync_run_summary({0, 8, 0, 0, 0}), "Uploaded 8.");
  EXPECT_EQ(vsm::sync_run_summary({2, 6, 56, 3, 0}),
            "Backed up 2, uploaded 6, 56 up to date, 3 failed.");
  EXPECT_EQ(vsm::sync_run_summary({0, 0, 73, 0, 0}), "All 73 games up to date.");
  EXPECT_EQ(vsm::sync_run_summary({1, 2, 0, 0, 9}),
            "Cancelled: backed up 1, uploaded 2, 9 games left.");
  EXPECT_EQ(vsm::sync_run_summary({0, 0, 0, 0, 9}), "Cancelled: 9 games left.");
  EXPECT_EQ(vsm::sync_run_summary({0, 0, 3, 0, 1}),
            "Cancelled: 3 up to date, 1 game left.");
  EXPECT_EQ(vsm::sync_run_summary({0, 0, 0, 0, 0}), "Nothing to do.");
}

void test_sync_all_hold_message_names_the_tab() {
  EXPECT_EQ(vsm::sync_all_hold_message("Vita"),
            "Keep holding: backup & upload all Vita saves...");
}

void test_display_backup_name_strips_zip_extension() {
  EXPECT_EQ(vsm::display_backup_name("2026-05-21 16-14.zip"), "2026-05-21 16-14");
  EXPECT_EQ(vsm::display_backup_name("no-extension"), "no-extension");
  EXPECT_EQ(vsm::display_backup_name(".zip"), "");
}

void test_drive_save_folder_names_carry_the_game_title() {
  EXPECT_EQ(vsm::drive_save_folder_name("PCSB00456", "FEZ"), "PCSB00456 FEZ");
  // No usable title: the folder stays the bare key, as older versions created it.
  EXPECT_EQ(vsm::drive_save_folder_name("PCSB00456", ""), "PCSB00456");
  EXPECT_EQ(vsm::drive_save_folder_name("PCSB00456", "PCSB00456"), "PCSB00456");
  // Titles are sanitized the same way local paths are.
  EXPECT_EQ(vsm::drive_save_folder_name("PCSE00120", "Persona/4? Golden"),
            "PCSE00120 Persona_4_ Golden");
}

void test_drive_folder_matching_accepts_bare_and_titled_names() {
  EXPECT_TRUE(vsm::drive_folder_matches_save("PCSB00456", "PCSB00456"));
  EXPECT_TRUE(vsm::drive_folder_matches_save("PCSB00456 FEZ", "PCSB00456"));
  EXPECT_TRUE(!vsm::drive_folder_matches_save("PCSB00456FEZ", "PCSB00456"));
  EXPECT_TRUE(!vsm::drive_folder_matches_save("PCSB004567", "PCSB00456"));
  EXPECT_TRUE(!vsm::drive_folder_matches_save("PCSB0045", "PCSB00456"));
  EXPECT_TRUE(!vsm::drive_folder_matches_save("", "PCSB00456"));
}

void test_drive_rename_metadata_json_escapes_the_name() {
  EXPECT_EQ(vsm::build_drive_rename_metadata_json("PCSB00456 \"FEZ\""),
            "{\"name\":\"PCSB00456 \\\"FEZ\\\"\"}");
}

} // namespace

int main() {
  test_timestamped_backup_name_uses_jksv_style_zip_name();
  test_backup_rows_merge_local_and_remote_by_timestamp_identity();
  test_backup_rows_do_not_merge_foreign_names_sharing_a_prefix();
  test_backup_name_identity_label_and_rename_helpers();
  test_backup_label_sanitizer_and_auto_conflict();
  test_path_component_normalization_replaces_unsafe_characters();
  test_sfo_parser_reads_title_strings();
  test_save_scanner_lists_direct_child_save_directories();
  test_selection_wraps_and_handles_empty_lists();
  test_grid_window_scrolls_only_when_selection_leaves_view();
  test_backup_archive_creates_timestamped_zip_snapshot();
  test_backup_archive_restores_snapshot_and_removes_stale_files();
  test_backup_archive_missing_file_does_not_clear_destination();
  test_backup_store_lists_local_zip_backups_newest_first();
  test_backup_store_builds_normalized_archive_path();
  test_google_auth_builds_device_code_request_body();
  test_google_auth_builds_token_poll_request_body();
  test_google_auth_builds_refresh_request_body();
  test_google_drive_root_folder_name_is_psv_saves();
  test_google_auth_parses_device_code_success_response();
  test_google_auth_parses_token_pending_error();
  test_google_auth_parses_token_success_response();
  test_google_config_parses_downloaded_client_json();
  test_embedded_google_client_credentials_default_empty();
  test_google_config_serializes_and_parses_token_cache();
  test_google_auth_builds_verification_qr_url_with_prefilled_code();
  test_google_drive_builds_folder_metadata_json();
  test_google_drive_builds_find_folder_query();
  test_google_drive_parses_first_file_id();
  test_google_drive_builds_upload_metadata_json();
  test_google_drive_builds_list_children_query();
  test_google_drive_builds_paged_index_queries();
  test_google_drive_parses_parents_and_page_token();
  test_google_drive_parses_single_object_upload_response();
  test_save_category_classification();
  test_saves_sort_by_display_name_case_insensitive();
  test_save_sort_modes_order_saves();
  test_utf8_truncation_and_system_font_detection();
  test_auto_backup_suffix_display_and_content_matching();
  test_app_settings_roundtrip_and_unknown_keys();
  test_sync_plan_decides_backup_and_upload_per_game();
  test_sync_all_confirm_message_states_scope();
  test_sync_run_summary_reports_results_and_cancellation();
  test_sync_all_hold_message_names_the_tab();
  test_display_backup_name_strips_zip_extension();
  test_drive_save_folder_names_carry_the_game_title();
  test_drive_folder_matching_accepts_bare_and_titled_names();
  test_drive_rename_metadata_json_escapes_the_name();

  std::cout << "vsm_core_tests passed\n";
  return 0;
}
