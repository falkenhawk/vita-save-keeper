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
  const vsm::BackupTimestamp timestamp{2026, 5, 21, 16, 14};

  EXPECT_EQ(vsm::make_timestamped_backup_name(timestamp), "2026-05-21 16-14.zip");
}

void test_remote_entries_are_prefixed_and_local_entries_are_plain() {
  const vsm::BackupEntry remote = vsm::BackupEntry::remote("2026-05-21 16-14.zip");
  const vsm::BackupEntry local = vsm::BackupEntry::local("2026-05-19 09-05.zip");

  EXPECT_EQ(remote.display_name(), "[GD] 2026-05-21 16-14.zip");
  EXPECT_EQ(local.display_name(), "2026-05-19 09-05.zip");
}

void test_backup_menu_order_matches_jksv_style() {
  const std::vector<vsm::BackupEntry> menu = vsm::build_backup_menu(
      {"2026-05-21 16-14.zip", "2026-05-20 22-31.zip"}, {"2026-05-19 09-05.zip"});

  EXPECT_EQ(menu.size(), static_cast<std::size_t>(4));
  EXPECT_TRUE(menu[0].kind == vsm::BackupEntryKind::NewBackup);
  EXPECT_EQ(menu[0].display_name(), "New Backup");
  EXPECT_EQ(menu[1].display_name(), "[GD] 2026-05-21 16-14.zip");
  EXPECT_EQ(menu[2].display_name(), "[GD] 2026-05-20 22-31.zip");
  EXPECT_EQ(menu[3].display_name(), "2026-05-19 09-05.zip");
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
      {2026, 5, 21, 16, 14},
  });

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.archive_path,
            (base / "backups" / "PCSE00120_ Persona_4" / "2026-05-21 16-14.zip").string());
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
  const vsm::DriveFileList files = vsm::parse_drive_file_list(
      "{\"nextPageToken\":\"tok-2\",\"files\":["
      "{\"id\":\"zip-1\",\"name\":\"2026-07-03 23-19.zip\",\"parents\":[\"folder-a\"]},"
      "{\"id\":\"orphan\",\"name\":\"loose.zip\"},"
      "{\"id\":\"zip-2\",\"name\":\"2026-07-01 10-00.zip\",\"parents\":[\"folder-b\"]}]}");

  EXPECT_TRUE(files.ok);
  EXPECT_EQ(files.next_page_token, "tok-2");
  EXPECT_EQ(files.files.size(), static_cast<std::size_t>(3));
  EXPECT_EQ(files.files[0].parent_id, "folder-a");
  EXPECT_EQ(files.files[1].parent_id, "");
  EXPECT_EQ(files.files[2].id, "zip-2");
  EXPECT_EQ(files.files[2].parent_id, "folder-b");
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

} // namespace

int main() {
  test_timestamped_backup_name_uses_jksv_style_zip_name();
  test_remote_entries_are_prefixed_and_local_entries_are_plain();
  test_backup_menu_order_matches_jksv_style();
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
  test_save_category_classification();
  test_saves_sort_by_display_name_case_insensitive();

  std::cout << "vsm_core_tests passed\n";
  return 0;
}
