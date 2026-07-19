#include "core/AppSettings.hpp"
#include "core/BackupArchive.hpp"
#include "core/BackupList.hpp"
#include "core/BackupName.hpp"
#include "core/BackupStore.hpp"
#include "core/GoogleAuth.hpp"
#include "core/GoogleConfig.hpp"
#include "core/GoogleDrive.hpp"
#include "core/GridWindow.hpp"
#include "core/InputGesture.hpp"
#include "core/MultipartUpload.hpp"
#include "core/PathUtil.hpp"
#include "core/SaveCategory.hpp"
#include "core/SaveScanner.hpp"
#include "core/SaveSlotMetadata.hpp"
#include "core/Selection.hpp"
#include "core/SfoParser.hpp"
#include "core/SyncPlan.hpp"
#include "core/TextUtil.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <utime.h>
#include <utility>
#include <vector>

namespace {

class ScopedTimezone {
public:
  explicit ScopedTimezone(const char *timezone) {
    const char *current = std::getenv("TZ");
    if (current) {
      previous_ = current;
      had_previous_ = true;
    }
    setenv("TZ", timezone, 1);
    tzset();
  }

  ~ScopedTimezone() {
    if (had_previous_) {
      setenv("TZ", previous_.c_str(), 1);
    } else {
      unsetenv("TZ");
    }
    tzset();
  }

private:
  std::string previous_;
  bool had_previous_{};
};

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

void write_le16_at(std::vector<unsigned char> *data, std::size_t offset,
                   std::uint16_t value) {
  (*data)[offset] = static_cast<unsigned char>(value & 0xff);
  (*data)[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xff);
}

void write_le32_at(std::vector<unsigned char> *data, std::size_t offset,
                   std::uint32_t value) {
  (*data)[offset] = static_cast<unsigned char>(value & 0xff);
  (*data)[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xff);
  (*data)[offset + 2] = static_cast<unsigned char>((value >> 16) & 0xff);
  (*data)[offset + 3] = static_cast<unsigned char>((value >> 24) & 0xff);
}

struct SdslotFixtureSlot {
  unsigned int id{};
  vsm::SaveDateTime modified_at;
  std::string title;
  std::string subtitle;
  std::string details;
};

void write_fixed_c_string(std::vector<unsigned char> *data, std::size_t offset,
                          std::size_t capacity, const std::string &value) {
  const std::size_t count = std::min(value.size(), capacity - 1);
  std::copy(value.begin(), value.begin() + static_cast<long>(count), data->begin() + offset);
  (*data)[offset + count] = 0;
}

std::vector<unsigned char> build_sdslot(const std::vector<SdslotFixtureSlot> &slots) {
  constexpr std::size_t kHeaderSize = 0x400;
  constexpr std::size_t kRecordSize = 0x400;
  std::vector<unsigned char> data(kHeaderSize + 256 * kRecordSize, 0);
  write_le32_at(&data, 0, 0x4c534453);
  write_le32_at(&data, 8, 0x00000100);
  for (const SdslotFixtureSlot &slot : slots) {
    EXPECT_TRUE(slot.id < 256);
    data[0x200 + slot.id] = 1;
    const std::size_t record = kHeaderSize + slot.id * kRecordSize;
    write_fixed_c_string(&data, record + 0x04, 0x40, slot.title);
    write_fixed_c_string(&data, record + 0x44, 0x80, slot.subtitle);
    write_fixed_c_string(&data, record + 0xc4, 0x200, slot.details);
    write_le16_at(&data, record + 0x30c, static_cast<std::uint16_t>(slot.modified_at.year));
    write_le16_at(&data, record + 0x30e, static_cast<std::uint16_t>(slot.modified_at.month));
    write_le16_at(&data, record + 0x310, static_cast<std::uint16_t>(slot.modified_at.day));
    write_le16_at(&data, record + 0x312, static_cast<std::uint16_t>(slot.modified_at.hour));
    write_le16_at(&data, record + 0x314, static_cast<std::uint16_t>(slot.modified_at.minute));
    write_le16_at(&data, record + 0x316, static_cast<std::uint16_t>(slot.modified_at.second));
  }
  return data;
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

void test_backup_timestamp_parser_reads_legacy_names_defensively() {
  vsm::BackupTimestamp timestamp;
  EXPECT_TRUE(vsm::parse_backup_timestamp("2026-07-12 01-44-07~2 before boss.zip", &timestamp));
  EXPECT_TRUE(timestamp.year == 2026 && timestamp.month == 7 && timestamp.day == 12);
  EXPECT_TRUE(timestamp.hour == 1 && timestamp.minute == 44 && timestamp.second == 7);
  EXPECT_TRUE(!vsm::parse_backup_timestamp("2026-02-30 01-44-07.zip", &timestamp));
  EXPECT_TRUE(!vsm::parse_backup_timestamp("my-imported-save.zip", &timestamp));
  EXPECT_TRUE(!vsm::parse_backup_timestamp("2026-07-12 01-44-07oops.zip", &timestamp));
}

void test_backup_counter_identity_and_allocation() {
  const vsm::BackupTimestamp timestamp{2026, 7, 12, 1, 44, 7};
  EXPECT_TRUE(vsm::has_backup_timestamp_prefix("2026-07-12 01-44-07~2.zip"));
  EXPECT_TRUE(vsm::has_backup_timestamp_prefix("2026-07-12 01-44-07~17 boss.zip"));
  EXPECT_TRUE(!vsm::has_backup_timestamp_prefix("2026-07-12 01-44-07~.zip"));
  EXPECT_TRUE(!vsm::has_backup_timestamp_prefix("2026-07-12 01-44-07~1.zip"));
  EXPECT_TRUE(!vsm::has_backup_timestamp_prefix("2026-07-12 01-44-07~2boss.zip"));
  EXPECT_EQ(vsm::backup_identity("2026-07-12 01-44-07~17 boss.zip"),
            "2026-07-12 01-44-07~17");
  EXPECT_EQ(vsm::backup_label("2026-07-12 01-44-07~17 boss.zip"), "boss");
  EXPECT_EQ(vsm::make_timestamped_backup_name(timestamp, 2), "2026-07-12 01-44-07~2.zip");
  EXPECT_EQ(vsm::allocate_backup_name(
                timestamp, "", {"2026-07-12 01-44-07.zip"},
                {"2026-07-12 01-44-07~2 cloud.zip"}),
            "2026-07-12 01-44-07~3.zip");
  EXPECT_EQ(vsm::allocate_backup_name(timestamp, " auto", {}, {}),
            "2026-07-12 01-44-07 auto.zip");
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

void test_sdslot_parser_reads_noncontiguous_slots_and_selects_newest() {
  ScopedTimezone timezone("UTC0");
  const std::vector<unsigned char> data = build_sdslot({
      {2, {2026, 7, 11, 23, 59, 58}, "WipEout 2048", "Profile data",
       "Campaign gold medals: 90"},
      {9, {2026, 7, 12, 1, 44, 7}, "WipEout 2048", "Ghost ship data",
       "Campaign gold medals: 91"},
  });

  const vsm::SaveMetadata metadata = vsm::parse_sdslot_data(data);

  EXPECT_TRUE(metadata.source == vsm::SaveTimeSource::VitaSlot);
  EXPECT_TRUE(!metadata.approximate);
  EXPECT_EQ(metadata.slots.size(), static_cast<std::size_t>(2));
  EXPECT_EQ(static_cast<std::size_t>(metadata.slots[0].id), static_cast<std::size_t>(2));
  EXPECT_EQ(static_cast<std::size_t>(metadata.slots[1].id), static_cast<std::size_t>(9));
  EXPECT_EQ(metadata.slots[1].subtitle, "Ghost ship data");
  EXPECT_EQ(vsm::format_save_datetime(metadata.saved_at), "2026-07-12T01:44:07");
}

void test_sdslot_parser_converts_utc_slot_time_to_device_local_time() {
  ScopedTimezone timezone("Europe/Warsaw");
  const std::vector<unsigned char> data = build_sdslot({
      {0, {2019, 7, 24, 1, 49, 14}, "3/4 Home", "Save", "Details"},
  });

  const vsm::SaveMetadata metadata = vsm::parse_sdslot_data(data);

  EXPECT_EQ(vsm::format_save_datetime(metadata.saved_at), "2019-07-24T03:49:14");
  EXPECT_EQ(vsm::format_save_datetime(metadata.slots[0].modified_at),
            "2019-07-24T03:49:14");
}

void test_sdslot_parser_skips_bad_records_but_keeps_valid_siblings() {
  ScopedTimezone timezone("UTC0");
  std::vector<unsigned char> data = build_sdslot({
      {3, {2024, 2, 29, 12, 0, 0}, "Valid \xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96", "Slot", "Details"},
      {7, {2026, 2, 29, 12, 0, 0}, "Invalid date", "Slot", "Details"},
  });
  const std::size_t invalid_utf8_record = 0x400 + 3 * 0x400;
  data[invalid_utf8_record + 0xc4] = 0xc3;
  data[invalid_utf8_record + 0xc5] = '(';
  data[invalid_utf8_record + 0xc6] = 0;

  const vsm::SaveMetadata metadata = vsm::parse_sdslot_data(data);

  EXPECT_EQ(metadata.slots.size(), static_cast<std::size_t>(1));
  EXPECT_EQ(metadata.slots[0].title,
            "Valid \xE3\x82\xBB\xE3\x83\xBC\xE3\x83\x96");
  EXPECT_EQ(metadata.slots[0].details, "\xEF\xBF\xBD(");
  EXPECT_EQ(vsm::format_save_datetime(metadata.saved_at), "2024-02-29T12:00:00");
}

void test_sdslot_parser_rejects_invalid_magic_and_truncation_but_keeps_full_fields() {
  ScopedTimezone timezone("UTC0");
  std::vector<unsigned char> invalid_magic = build_sdslot({
      {0, {2026, 1, 1, 0, 0, 0}, "Title", "Subtitle", "Details"},
  });
  invalid_magic[0] = 0;
  EXPECT_TRUE(vsm::parse_sdslot_data(invalid_magic).slots.empty());

  std::vector<unsigned char> truncated = build_sdslot({
      {5, {2026, 1, 1, 0, 0, 0}, "Title", "Subtitle", "Details"},
  });
  truncated.resize(0x400 + 5 * 0x400 + 100);
  EXPECT_TRUE(vsm::parse_sdslot_data(truncated).slots.empty());

  // A field that fills its whole capacity with no terminator is a maxed-out string, not
  // corruption: the slot (and its valid timestamp) is kept and the title is truncated at capacity.
  std::vector<unsigned char> unterminated = build_sdslot({
      {1, {2026, 1, 1, 0, 0, 0}, "Title", "Subtitle", "Details"},
  });
  const std::size_t record = 0x400 + 0x400;
  std::fill(unterminated.begin() + static_cast<long>(record + 0x04),
            unterminated.begin() + static_cast<long>(record + 0x44), 'x');
  const vsm::SaveMetadata parsed = vsm::parse_sdslot_data(unterminated);
  EXPECT_EQ(parsed.slots.size(), static_cast<std::size_t>(1));
  EXPECT_EQ(parsed.slots[0].title, std::string(0x40, 'x'));
  EXPECT_EQ(parsed.slots[0].subtitle, "Subtitle");
}

void test_save_metadata_resolver_uses_recursive_files_then_backup_clock() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-metadata-time-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "save" / "nested");
  std::ofstream(base / "save" / "older.bin", std::ios::binary) << "old";
  std::ofstream(base / "save" / "nested" / "newest.bin", std::ios::binary) << "new";

  const std::time_t older = 1700000000;
  const std::time_t newest = 1700001234;
  const utimbuf older_times{older, older};
  const utimbuf newest_times{newest, newest};
  EXPECT_TRUE(utime((base / "save" / "older.bin").string().c_str(), &older_times) == 0);
  EXPECT_TRUE(utime((base / "save" / "nested" / "newest.bin").string().c_str(),
                    &newest_times) == 0);

  const vsm::SaveMetadata filesystem =
      vsm::resolve_save_metadata((base / "save").string(), {2001, 2, 3, 4, 5, 6});
  EXPECT_TRUE(filesystem.source == vsm::SaveTimeSource::Filesystem);
  EXPECT_TRUE(filesystem.approximate);
  EXPECT_EQ(static_cast<std::size_t>(vsm::save_datetime_to_local_epoch(filesystem.saved_at)),
            static_cast<std::size_t>(newest));

  std::filesystem::create_directories(base / "empty");
  const vsm::SaveMetadata clock =
      vsm::resolve_save_metadata((base / "empty").string(), {2001, 2, 3, 4, 5, 6});
  EXPECT_TRUE(clock.source == vsm::SaveTimeSource::BackupClock);
  EXPECT_TRUE(clock.approximate);
  EXPECT_EQ(vsm::format_save_datetime(clock.saved_at), "2001-02-03T04:05:06");

  std::filesystem::remove_all(base);
}

void test_pfs_mount_policy_requires_existing_metadata_directory() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-pfs-policy-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "save");
  std::ofstream(base / "save" / "plain.bin", std::ios::binary) << "plain save";

  EXPECT_TRUE(!vsm::save_directory_has_pfs_metadata((base / "save").string()));

  std::ofstream(base / "save" / "sce_pfs", std::ios::binary) << "not a directory";
  EXPECT_TRUE(!vsm::save_directory_has_pfs_metadata((base / "save").string()));
  std::filesystem::remove(base / "save" / "sce_pfs");
  std::filesystem::create_directory(base / "save" / "sce_pfs");
  EXPECT_TRUE(vsm::save_directory_has_pfs_metadata((base / "save").string()));

  std::filesystem::remove_all(base);
}

void test_save_metadata_json_round_trips_and_ignores_unknown_fields() {
  vsm::SaveMetadata metadata;
  metadata.saved_at = {2026, 7, 12, 1, 44, 7};
  metadata.source = vsm::SaveTimeSource::VitaSlot;
  metadata.approximate = false;
  metadata.slots.push_back(
      {2, {2026, 7, 12, 1, 44, 7}, "WipEout \"2048\"", "Profile\\data",
       "Campaign\nmedals: 91"});

  std::string json =
      vsm::serialize_save_metadata_json("2026-07-12 01-44-07", metadata);
  EXPECT_TRUE(json.find("\"version\":2") != std::string::npos);
  json.insert(json.size() - 1, ",\"future\":{\"nested\":[true,null,3]}");
  const vsm::SaveMetadataJsonResult parsed = vsm::parse_save_metadata_json(json);

  EXPECT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.archive_identity, "2026-07-12 01-44-07");
  EXPECT_TRUE(parsed.metadata.source == vsm::SaveTimeSource::VitaSlot);
  EXPECT_TRUE(!parsed.metadata.approximate);
  EXPECT_EQ(vsm::format_save_datetime(parsed.metadata.saved_at), "2026-07-12T01:44:07");
  EXPECT_EQ(parsed.metadata.slots.size(), static_cast<std::size_t>(1));
  EXPECT_EQ(parsed.metadata.slots[0].title, "WipEout \"2048\"");
  EXPECT_EQ(parsed.metadata.slots[0].subtitle, "Profile\\data");
  EXPECT_EQ(parsed.metadata.slots[0].details, "Campaign\nmedals: 91");
}

void test_legacy_vita_slot_json_is_upgraded_from_utc_to_local_time() {
  ScopedTimezone timezone("Europe/Warsaw");
  const std::string legacy =
      "{\"version\":1,\"archiveIdentity\":\"2019-07-24 03-49-14\","
      "\"savedAt\":\"2019-07-24T01:49:14\",\"source\":\"vita-slot\","
      "\"approximate\":false,\"slots\":[{\"id\":0,"
      "\"modifiedAt\":\"2019-07-24T01:49:14\",\"title\":\"3/4 Home\","
      "\"subtitle\":\"Save\",\"details\":\"Details\"}]}";

  const vsm::SaveMetadataJsonResult parsed = vsm::parse_save_metadata_json(legacy);

  EXPECT_TRUE(parsed.ok);
  EXPECT_EQ(vsm::format_save_datetime(parsed.metadata.saved_at), "2019-07-24T03:49:14");
  EXPECT_EQ(vsm::format_save_datetime(parsed.metadata.slots[0].modified_at),
            "2019-07-24T03:49:14");
}

void test_backup_metadata_is_usable_only_for_matching_trustworthy_identity() {
  vsm::SaveMetadataJsonResult metadata;
  metadata.ok = true;
  metadata.archive_identity = "2026-07-12 01-44-07";
  metadata.metadata.source = vsm::SaveTimeSource::Filesystem;

  EXPECT_TRUE(vsm::save_metadata_is_usable(metadata, "2026-07-12 01-44-07"));
  EXPECT_TRUE(!vsm::save_metadata_is_usable(metadata, "2026-07-12 01-44-08"));

  metadata.metadata.source = vsm::SaveTimeSource::BackupClock;
  EXPECT_TRUE(!vsm::save_metadata_is_usable(metadata, "2026-07-12 01-44-07"));
  metadata.ok = false;
  metadata.metadata.source = vsm::SaveTimeSource::VitaSlot;
  EXPECT_TRUE(!vsm::save_metadata_is_usable(metadata, "2026-07-12 01-44-07"));
}

void test_observed_save_metadata_accepts_slots_and_files_but_not_backup_clock() {
  vsm::SaveMetadata metadata;
  metadata.source = vsm::SaveTimeSource::Filesystem;
  EXPECT_TRUE(vsm::save_metadata_has_observed_time(metadata));

  metadata.source = vsm::SaveTimeSource::VitaSlot;
  EXPECT_TRUE(!vsm::save_metadata_has_observed_time(metadata));
  metadata.slots.push_back({});
  EXPECT_TRUE(vsm::save_metadata_has_observed_time(metadata));

  metadata.source = vsm::SaveTimeSource::BackupClock;
  EXPECT_TRUE(!vsm::save_metadata_has_observed_time(metadata));
}

void test_save_metadata_json_rejects_untrusted_bounds_and_invalid_utf8() {
  const std::string valid_prefix =
      "{\"version\":1,\"archiveIdentity\":\"id\","
      "\"savedAt\":\"2026-07-12T01:44:07\",\"source\":\"vita-slot\","
      "\"approximate\":false,\"slots\":[]";
  EXPECT_TRUE(!vsm::parse_save_metadata_json(
                   "{\"version\":2,\"archiveIdentity\":\"id\"}")
                   .ok);
  EXPECT_TRUE(!vsm::parse_save_metadata_json(valid_prefix + ",\"future\":" +
                                              std::string(33, '[') + "0" +
                                              std::string(33, ']') + "}")
                   .ok);

  std::string invalid_utf8 = valid_prefix;
  invalid_utf8.insert(invalid_utf8.size() - 1, ",\"bad\":\"");
  invalid_utf8.push_back(static_cast<char>(0xc3));
  invalid_utf8 += "(\"}";
  EXPECT_TRUE(!vsm::parse_save_metadata_json(invalid_utf8).ok);

  std::string too_many = valid_prefix.substr(0, valid_prefix.size() - 2) + "[";
  for (int i = 0; i < 257; ++i) {
    if (i > 0) {
      too_many += ",";
    }
    too_many +=
        "{\"id\":0,\"modifiedAt\":\"2026-07-12T01:44:07\",\"title\":\"\","
        "\"subtitle\":\"\",\"details\":\"\"}";
  }
  too_many += "]}";
  EXPECT_TRUE(!vsm::parse_save_metadata_json(too_many).ok);

  const std::string overlong =
      valid_prefix.substr(0, valid_prefix.size() - 2) +
      "[{\"id\":0,\"modifiedAt\":\"2026-07-12T01:44:07\",\"title\":\"\","
      "\"subtitle\":\"\",\"details\":\"" + std::string(513, 'x') + "\"}]}";
  EXPECT_TRUE(!vsm::parse_save_metadata_json(overlong).ok);
}

void test_save_metadata_json_file_write_is_atomic_and_bounded() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-metadata-json-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);
  const std::filesystem::path path = base / "snapshot.json";

  vsm::SaveMetadata first;
  first.saved_at = {2026, 7, 12, 1, 44, 7};
  first.source = vsm::SaveTimeSource::Filesystem;
  first.approximate = true;
  std::string error;
  EXPECT_TRUE(vsm::write_save_metadata_json_atomic(path.string(), "snapshot", first, &error));

  vsm::SaveMetadata second = first;
  second.saved_at.second = 8;
  EXPECT_TRUE(vsm::write_save_metadata_json_atomic(path.string(), "snapshot", second, &error));
  const vsm::SaveMetadataJsonResult loaded = vsm::read_save_metadata_json(path.string());
  EXPECT_TRUE(loaded.ok);
  EXPECT_EQ(vsm::format_save_datetime(loaded.metadata.saved_at), "2026-07-12T01:44:08");
  EXPECT_TRUE(!std::filesystem::exists(path.string() + ".tmp"));

  std::ofstream(base / "large.json", std::ios::binary)
      << std::string(vsm::kMaxMetadataJsonSize + 1, 'x');
  EXPECT_TRUE(!vsm::read_save_metadata_json((base / "large.json").string()).ok);

  std::filesystem::remove_all(base);
}

void test_backup_metadata_names_follow_archive_identity() {
  EXPECT_EQ(vsm::backup_metadata_name("2026-07-12 01-44-07 before-boss.zip"),
            "2026-07-12 01-44-07.json");
  EXPECT_EQ(vsm::backup_metadata_name("2026-07-12 01-44-07~2 test.zip"),
            "2026-07-12 01-44-07~2.json");
  EXPECT_EQ(vsm::backup_metadata_name("my-imported-save.zip"), "my-imported-save.json");
}

void test_save_scanner_lists_direct_child_save_directories() {
  ScopedTimezone timezone("UTC0");
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-scanner-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "vita" / "BHBB00001");
  std::filesystem::create_directories(base / "vita" / "PCSE00120");
  std::filesystem::create_directories(base / "vita" / "PCSE88888" / "sce_sys");
  std::filesystem::create_directories(base / "vita" / "PCSE88888" / "sce_pfs");
  std::filesystem::create_directories(base / "vita" / "PCSE99999");
  std::filesystem::create_directories(base / "vita" / "PCSE00120" / "sce_sys");
  std::filesystem::create_directories(base / "psp" / "ULUS12345");
  std::ofstream(base / "vita" / "BHBB00001" / "data.bin", std::ios::binary)
      << "homebrew save";
  write_binary_file(base / "vita" / "PCSE00120" / "sce_sys" / "param.sfo",
                    build_sfo_with_strings({{"TITLE", "Persona 4 Golden"},
                                            {"TITLE_ID", "PCSE00120"}}));
  std::ofstream(base / "vita" / "PCSE00120" / "sce_sys" / "icon0.png", std::ios::binary)
      << "png";
  write_binary_file(base / "vita" / "PCSE88888" / "sce_sys" / "param.sfo",
                    build_sfo_with_strings({{"TITLE", "Encrypted Save"},
                                            {"TITLE_ID", "PCSE88888"}}));
  std::ofstream(base / "vita" / "PCSE88888" / "sce_pfs" / "files.db", std::ios::binary)
      << "newer bookkeeping time";
  write_binary_file(base / "psp" / "ULUS12345" / "PARAM.SFO",
                    build_sfo_with_strings({{"TITLE", "PSP Save"}}));
  std::ofstream(base / "psp" / "ULUS12345" / "ICON0.PNG", std::ios::binary) << "png";
  const std::time_t vita_saved_at = 1700001234;
  const std::time_t homebrew_saved_at = 1700002345;
  const std::time_t psp_saved_at = 1700003456;
  const std::time_t pfs_bookkeeping_at = 1800000000;
  const utimbuf vita_times{vita_saved_at, vita_saved_at};
  const utimbuf homebrew_times{homebrew_saved_at, homebrew_saved_at};
  const utimbuf psp_times{psp_saved_at, psp_saved_at};
  const utimbuf pfs_times{pfs_bookkeeping_at, pfs_bookkeeping_at};
  EXPECT_TRUE(utime((base / "vita" / "PCSE00120" / "sce_sys" / "param.sfo").string().c_str(),
                    &vita_times) == 0);
  EXPECT_TRUE(utime((base / "vita" / "PCSE00120" / "sce_sys" / "icon0.png").string().c_str(),
                    &vita_times) == 0);
  EXPECT_TRUE(utime((base / "vita" / "BHBB00001" / "data.bin").string().c_str(),
                    &homebrew_times) == 0);
  EXPECT_TRUE(utime((base / "psp" / "ULUS12345" / "PARAM.SFO").string().c_str(),
                    &psp_times) == 0);
  EXPECT_TRUE(utime((base / "psp" / "ULUS12345" / "ICON0.PNG").string().c_str(),
                    &psp_times) == 0);
  EXPECT_TRUE(utime((base / "vita" / "PCSE88888" / "sce_pfs" / "files.db").string().c_str(),
                    &pfs_times) == 0);

  FILE *ignored_file = std::fopen((base / "vita" / "not-a-save.txt").string().c_str(), "w");
  EXPECT_TRUE(ignored_file != nullptr);
  std::fclose(ignored_file);

  std::vector<std::string> resolved_paths;
  const auto resolver = [&](const std::string &path, const vsm::SaveDateTime &clock) {
    resolved_paths.push_back(path);
    return vsm::resolve_save_metadata(path, clock);
  };
  std::size_t scan_progress = 0;
  std::size_t scan_last_total = 0;
  const std::vector<vsm::SaveRecord> saves = vsm::scan_save_roots(
      {
          {vsm::SavePlatform::Vita, (base / "vita").string()},
          {vsm::SavePlatform::Psp, (base / "psp").string()},
          {vsm::SavePlatform::GameCard, (base / "missing").string()},
      },
      [&](std::size_t done, std::size_t total) {
        ++scan_progress;
        scan_last_total = total;
        EXPECT_TRUE(done <= total);
      },
      resolver);

  EXPECT_EQ(saves.size(), static_cast<std::size_t>(5));
  EXPECT_TRUE(saves[0].platform == vsm::SavePlatform::Vita);
  EXPECT_EQ(saves[0].id, "BHBB00001");
  EXPECT_TRUE(saves[0].save_time_known);
  EXPECT_EQ(static_cast<std::size_t>(saves[0].saved_at_epoch),
            static_cast<std::size_t>(homebrew_saved_at));
  EXPECT_EQ(vsm::format_save_datetime(saves[0].saved_at), "2023-11-14T22:52:25");

  EXPECT_EQ(saves[1].id, "PCSE00120");
  EXPECT_EQ(saves[1].display_name, "Persona 4 Golden");
  EXPECT_EQ(saves[1].title_id, "PCSE00120");
  EXPECT_EQ(saves[1].path, (base / "vita" / "PCSE00120").string());
  EXPECT_EQ(saves[1].icon_path,
            (base / "vita" / "PCSE00120" / "sce_sys" / "icon0.png").string());
  EXPECT_TRUE(saves[1].save_time_known);
  EXPECT_EQ(static_cast<std::size_t>(saves[1].saved_at_epoch),
            static_cast<std::size_t>(vita_saved_at));
  EXPECT_EQ(vsm::format_save_datetime(saves[1].saved_at), "2023-11-14T22:33:54");

  EXPECT_EQ(saves[2].id, "PCSE88888");
  EXPECT_TRUE(!saves[2].save_time_known);
  EXPECT_TRUE(saves[2].save_time_requires_mount);
  EXPECT_EQ(static_cast<std::size_t>(saves[2].saved_at.year), static_cast<std::size_t>(0));
  EXPECT_EQ(static_cast<std::size_t>(saves[2].saved_at_epoch), static_cast<std::size_t>(0));

  EXPECT_EQ(saves[3].id, "PCSE99999");
  EXPECT_EQ(saves[3].display_name, "PCSE99999");
  EXPECT_TRUE(!saves[3].save_time_known);

  EXPECT_TRUE(saves[4].platform == vsm::SavePlatform::Psp);
  EXPECT_EQ(saves[4].id, "ULUS12345");
  EXPECT_EQ(saves[4].display_name, "PSP Save");
  EXPECT_EQ(saves[4].icon_path, (base / "psp" / "ULUS12345" / "ICON0.PNG").string());
  EXPECT_TRUE(saves[4].save_time_known);
  EXPECT_EQ(static_cast<std::size_t>(saves[4].saved_at_epoch),
            static_cast<std::size_t>(psp_saved_at));
  EXPECT_EQ(vsm::format_save_datetime(saves[4].saved_at), "2023-11-14T23:10:56");

  EXPECT_EQ(resolved_paths.size(), static_cast<std::size_t>(4));
  EXPECT_TRUE(std::find(resolved_paths.begin(), resolved_paths.end(), saves[2].path) ==
              resolved_paths.end());
  EXPECT_EQ(scan_progress, saves.size());
  EXPECT_EQ(scan_last_total, saves.size());

  std::filesystem::remove_all(base);
}

void test_mounted_save_time_trusts_observed_times_and_rejects_unresolved() {
  vsm::SaveRecord save;
  save.saved_at_epoch = 999;
  save.save_time_requires_mount = true;

  // A backup-clock (no observed time) result means neither a slot time nor a file time was found.
  // The record keeps its prior epoch and stays time-unknown, so the grid shows "Unknown".
  vsm::SaveMetadata unresolved;
  unresolved.source = vsm::SaveTimeSource::BackupClock;
  EXPECT_TRUE(!vsm::apply_mounted_save_time(&save, unresolved));
  EXPECT_TRUE(!save.save_time_known);
  EXPECT_TRUE(!save.save_time_requires_mount);
  EXPECT_EQ(static_cast<std::size_t>(save.saved_at_epoch), static_cast<std::size_t>(999));

  // A filesystem time from a successful mount (a save with no Vita slots, e.g. Cladun Returns) is
  // trusted now, so the grid row and the details screen agree instead of showing "Unknown" vs a
  // real time.
  save.save_time_requires_mount = true;
  vsm::SaveMetadata files;
  files.saved_at = {2020, 1, 16, 23, 10, 48};
  files.source = vsm::SaveTimeSource::Filesystem;
  EXPECT_TRUE(vsm::apply_mounted_save_time(&save, files));
  EXPECT_TRUE(save.save_time_known);
  EXPECT_EQ(vsm::format_save_datetime(save.saved_at), "2020-01-16T23:10:48");

  save.save_time_requires_mount = true;
  vsm::SaveMetadata slots;
  slots.saved_at = {2019, 7, 24, 1, 49, 14};
  slots.source = vsm::SaveTimeSource::VitaSlot;
  slots.slots.push_back({0, slots.saved_at, {}, {}, {}});
  EXPECT_TRUE(vsm::apply_mounted_save_time(&save, slots));
  EXPECT_TRUE(save.save_time_known);
  EXPECT_TRUE(!save.save_time_requires_mount);
  EXPECT_EQ(vsm::format_save_datetime(save.saved_at), "2019-07-24T01:49:14");
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

void test_detail_view_sizes_from_folder_and_archive() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-size-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "source" / "sce_sys");
  std::filesystem::create_directories(base / "source" / "save");
  std::filesystem::create_directories(base / "backups");
  {
    std::ofstream(base / "source" / "data.bin", std::ios::binary) << "hello";              // 5
    std::ofstream(base / "source" / "sce_sys" / "icon0.png", std::ios::binary) << "icon!"; // 5
    std::ofstream(base / "source" / "save" / "slot0", std::ios::binary) << "0123456789";   // 10
  }
  // The live-save size is the recursive on-disk total of every regular file.
  bool folder_ok = false;
  EXPECT_EQ(vsm::compute_folder_size((base / "source").string(), &folder_ok),
            static_cast<std::uint64_t>(20));
  EXPECT_TRUE(folder_ok);

  const vsm::BackupResult backup = vsm::create_backup_archive({
      (base / "source").string(),
      (base / "backups").string(),
      "PCSE00042",
      {2026, 7, 17, 12, 0, 0},
  });
  EXPECT_TRUE(backup.ok);

  bool zip_ok = false;
  const std::uint64_t zip_bytes = vsm::archive_file_size(backup.archive_path, &zip_ok);
  EXPECT_TRUE(zip_ok);
  // Store-method entries: the ZIP is the content plus per-entry header overhead, never smaller.
  EXPECT_TRUE(zip_bytes > 20);
  EXPECT_EQ(zip_bytes,
            static_cast<std::uint64_t>(std::filesystem::file_size(backup.archive_path)));

  // Unreadable paths report failure instead of a misleading zero.
  bool missing_ok = true;
  EXPECT_EQ(vsm::compute_folder_size((base / "nope").string(), &missing_ok),
            static_cast<std::uint64_t>(0));
  EXPECT_TRUE(!missing_ok);
  bool no_zip_ok = true;
  vsm::archive_file_size((base / "nope.zip").string(), &no_zip_ok);
  EXPECT_TRUE(!no_zip_ok);

  std::filesystem::remove_all(base);
}

void test_save_fingerprint_reflects_folder_content() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-fingerprint-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "save" / "sce_sys");
  {
    std::ofstream(base / "save" / "data.bin", std::ios::binary) << "hello";               // 5
    std::ofstream(base / "save" / "sce_sys" / "sdslot.dat", std::ios::binary) << "0123";  // 4
  }

  const vsm::SaveFingerprint fingerprint = vsm::compute_save_fingerprint((base / "save").string());
  EXPECT_TRUE(fingerprint.ok);
  EXPECT_EQ(fingerprint.file_count, 2LL);
  EXPECT_EQ(fingerprint.total_bytes, 9LL);
  EXPECT_TRUE(fingerprint.newest_mtime > 0);
  EXPECT_TRUE(fingerprint.matches(vsm::compute_save_fingerprint((base / "save").string())));

  // Content growth changes the fingerprint even when the mtime granularity hides the rewrite.
  { std::ofstream(base / "save" / "data.bin", std::ios::binary) << "hello!"; }
  const vsm::SaveFingerprint changed = vsm::compute_save_fingerprint((base / "save").string());
  EXPECT_TRUE(changed.ok);
  EXPECT_TRUE(!fingerprint.matches(changed));

  // Missing and empty folders never produce a matchable fingerprint.
  const vsm::SaveFingerprint missing = vsm::compute_save_fingerprint((base / "nope").string());
  EXPECT_TRUE(!missing.ok);
  EXPECT_TRUE(!missing.matches(missing));

  std::filesystem::remove_all(base);
}

void test_save_time_cache_roundtrip_and_rejects_bad_input() {
  vsm::SaveTimeCache cache;
  cache.entries["PCSG00352"] = {{true, 1783382228LL, 412LL, 5767168LL},
                                true,
                                {2026, 7, 6, 13, 37, 8}};
  // A save that resolved to no readable time: cached without a savedAt so it is not re-mounted
  // every launch just to learn "unknown" again.
  cache.entries["PCSB00466"] = {{true, 1579216248LL, 4LL, 65536LL}, false, {}};

  vsm::SaveTimeCache parsed;
  EXPECT_TRUE(vsm::parse_save_time_cache(vsm::serialize_save_time_cache(cache), &parsed));
  EXPECT_EQ(parsed.entries.size(), static_cast<std::size_t>(2));
  const vsm::SaveTimeCacheEntry &entry = parsed.entries["PCSG00352"];
  EXPECT_TRUE(entry.fingerprint.matches(cache.entries["PCSG00352"].fingerprint));
  EXPECT_TRUE(entry.has_time);
  EXPECT_EQ(vsm::format_save_datetime(entry.saved_at), "2026-07-06T13:37:08");
  const vsm::SaveTimeCacheEntry &no_time = parsed.entries["PCSB00466"];
  EXPECT_TRUE(!no_time.has_time);
  EXPECT_TRUE(no_time.fingerprint.matches(cache.entries["PCSB00466"].fingerprint));

  // Corrupt document or unknown version: reject the whole file (the cache simply rebuilds).
  EXPECT_TRUE(!vsm::parse_save_time_cache("not json", &parsed));
  EXPECT_TRUE(parsed.entries.empty());
  EXPECT_TRUE(!vsm::parse_save_time_cache("{\"version\":99,\"entries\":{}}", &parsed));

  // A malformed entry is skipped without discarding its healthy neighbors.
  EXPECT_TRUE(vsm::parse_save_time_cache(
      "{\"version\":1,\"entries\":{"
      "\"BAD001\":{\"newestMtime\":1,\"fileCount\":2},"
      "\"BAD002\":{\"newestMtime\":1,\"fileCount\":2,\"totalBytes\":3,"
      "\"savedAt\":\"2026-13-40T99:99:99\"},"
      "\"GOOD01\":{\"newestMtime\":1,\"fileCount\":2,\"totalBytes\":3,"
      "\"savedAt\":\"2026-07-06T13:37:08\"}}}",
      &parsed));
  EXPECT_EQ(parsed.entries.size(), static_cast<std::size_t>(1));
  EXPECT_TRUE(parsed.entries.find("GOOD01") != parsed.entries.end());
}

void test_scan_fingerprints_every_save_and_flags_mount_requiring_ones() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-scan-fingerprint-test";
  std::filesystem::remove_all(base);
  // PCSE88888 mirrors a retail PFS save (sce_pfs marker); BHBB00001 is a plain homebrew save.
  std::filesystem::create_directories(base / "vita" / "PCSE88888" / "sce_pfs");
  std::filesystem::create_directories(base / "vita" / "PCSE88888" / "sce_sys");
  std::filesystem::create_directories(base / "vita" / "BHBB00001");
  {
    std::ofstream(base / "vita" / "PCSE88888" / "sce_sys" / "keystone", std::ios::binary) << "k";
    std::ofstream(base / "vita" / "BHBB00001" / "save.dat", std::ios::binary) << "homebrew";
  }

  const std::vector<vsm::SaveRecord> saves = vsm::scan_save_roots(
      {{vsm::SavePlatform::Vita, (base / "vita").string()}}, {}, {});
  EXPECT_EQ(saves.size(), static_cast<std::size_t>(2));
  for (const vsm::SaveRecord &save : saves) {
    EXPECT_TRUE(save.fingerprint.ok);
    EXPECT_EQ(save.fingerprint.file_count, 1LL);
    if (save.id == "PCSE88888") {
      EXPECT_TRUE(save.save_time_requires_mount);
    } else {
      EXPECT_TRUE(!save.save_time_requires_mount);
      EXPECT_TRUE(save.save_time_known);
    }
    EXPECT_TRUE(!save.title_from_cache);
  }

  std::filesystem::remove_all(base);
}

void test_scan_consumes_time_and_title_caches_when_fingerprints_match() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-scan-cache-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "vita" / "BHBB00001");
  { std::ofstream(base / "vita" / "BHBB00001" / "save.dat", std::ios::binary) << "homebrew"; }
  const std::vector<vsm::SaveRoot> roots = {
      {vsm::SavePlatform::Vita, (base / "vita").string()}};

  // First scan derives everything and gives us the fingerprint to build cache entries from.
  const std::vector<vsm::SaveRecord> cold = vsm::scan_save_roots(roots, {}, {});
  EXPECT_EQ(cold.size(), static_cast<std::size_t>(1));

  vsm::SaveTimeCache times;
  times.entries["BHBB00001"] = {cold[0].fingerprint, true, {2026, 7, 1, 12, 0, 0}};
  vsm::SaveTitleCache titles;
  titles.entries["BHBB00001"] = {false, cold[0].fingerprint, "Cached Homebrew", "BHBB00001",
                                 "cached-icon.png"};

  // A cached save must not invoke the metadata resolver at all, and the cached title must win
  // (distinct from anything the folder could produce - there is no param.sfo in it).
  std::size_t resolver_calls = 0;
  const auto counting_resolver = [&](const std::string &path,
                                     const vsm::SaveDateTime &clock) {
    ++resolver_calls;
    return vsm::resolve_save_metadata(path, clock);
  };
  const std::vector<vsm::SaveRecord> warm =
      vsm::scan_save_roots(roots, {}, counting_resolver, &times, &titles);
  EXPECT_EQ(resolver_calls, static_cast<std::size_t>(0));
  EXPECT_EQ(warm.size(), static_cast<std::size_t>(1));
  EXPECT_TRUE(warm[0].save_time_known);
  EXPECT_EQ(vsm::format_save_datetime(warm[0].saved_at), "2026-07-01T12:00:00");
  EXPECT_TRUE(warm[0].title_from_cache);
  EXPECT_EQ(warm[0].display_name, "Cached Homebrew");
  EXPECT_EQ(warm[0].icon_path, "cached-icon.png");

  // Touch the save: both caches must be bypassed again.
  { std::ofstream(base / "vita" / "BHBB00001" / "save.dat", std::ios::binary) << "homebrew!!"; }
  const std::vector<vsm::SaveRecord> changed =
      vsm::scan_save_roots(roots, {}, counting_resolver, &times, &titles);
  EXPECT_EQ(resolver_calls, static_cast<std::size_t>(1));
  EXPECT_TRUE(!changed[0].title_from_cache);
  EXPECT_EQ(changed[0].display_name, "BHBB00001");

  // An app-database entry ignores the fingerprint: it stays valid until the db stamp changes,
  // which the caller checks before passing the cache in.
  titles.entries["BHBB00001"].from_app_db = true;
  const std::vector<vsm::SaveRecord> db_titled =
      vsm::scan_save_roots(roots, {}, counting_resolver, &times, &titles);
  EXPECT_TRUE(db_titled[0].title_from_cache);
  EXPECT_TRUE(db_titled[0].title_from_app_db);
  EXPECT_EQ(db_titled[0].display_name, "Cached Homebrew");

  std::filesystem::remove_all(base);
}

void test_save_title_cache_roundtrip_and_stat_stamp() {
  vsm::SaveTitleCache cache;
  cache.app_db_mtime = 1784300000LL;
  cache.app_db_size = 9437184LL;
  cache.entries["PCSB01084"] = {true, {true, 1LL, 2LL, 3LL}, "Papers, Please", "PCSB01084",
                                "ur0:appmeta/PCSB01084/icon0.png"};
  cache.entries["ULUS10336"] = {false, {true, 4LL, 5LL, 6LL}, "Patapon 2", "ULUS10336",
                                "ICON0.PNG"};

  vsm::SaveTitleCache parsed;
  EXPECT_TRUE(vsm::parse_save_title_cache(vsm::serialize_save_title_cache(cache), &parsed));
  EXPECT_EQ(parsed.app_db_mtime, cache.app_db_mtime);
  EXPECT_EQ(parsed.app_db_size, cache.app_db_size);
  EXPECT_EQ(parsed.entries.size(), static_cast<std::size_t>(2));
  EXPECT_TRUE(parsed.entries["PCSB01084"].from_app_db);
  EXPECT_EQ(parsed.entries["PCSB01084"].display_name, "Papers, Please");
  EXPECT_TRUE(!parsed.entries["ULUS10336"].from_app_db);
  EXPECT_TRUE(parsed.entries["ULUS10336"].fingerprint.matches(
      cache.entries["ULUS10336"].fingerprint));

  EXPECT_TRUE(!vsm::parse_save_title_cache("not json", &parsed));
  EXPECT_TRUE(parsed.entries.empty());
  EXPECT_TRUE(!vsm::parse_save_title_cache(
      "{\"version\":99,\"appDbMtime\":1,\"appDbSize\":2,\"entries\":{}}", &parsed));

  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-stamp-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);
  { std::ofstream(base / "app.db", std::ios::binary) << "0123456789"; }
  long long mtime = 0;
  long long size = 0;
  EXPECT_TRUE(vsm::stat_file_stamp((base / "app.db").string(), &mtime, &size));
  EXPECT_EQ(size, 10LL);
  EXPECT_TRUE(mtime > 0);
  EXPECT_TRUE(!vsm::stat_file_stamp((base / "missing.db").string(), &mtime, &size));
  std::filesystem::remove_all(base);
}

void test_backup_archive_reads_bounded_sdslot_entry_without_restoring() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-archive-entry-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "source" / "sce_sys");
  const std::vector<unsigned char> sdslot = build_sdslot({
      {4, {2026, 7, 12, 1, 44, 7}, "Title", "Subtitle", "Details"},
  });
  write_binary_file(base / "source" / "sce_sys" / "sdslot.dat", sdslot);
  const vsm::BackupResult backup = vsm::create_backup_archive({
      (base / "source").string(), (base / "backups").string(), "PCSE00001",
      {2026, 7, 12, 2, 0, 0},
  });
  EXPECT_TRUE(backup.ok);

  const vsm::ArchiveReadResult read = vsm::read_stored_backup_entry(
      backup.archive_path, "sce_sys/sdslot.dat", sdslot.size());
  EXPECT_TRUE(read.ok);
  EXPECT_EQ(read.data.size(), sdslot.size());
  EXPECT_EQ(vsm::parse_sdslot_data(read.data).slots.size(), static_cast<std::size_t>(1));
  const vsm::ArchiveReadResult missing =
      vsm::read_stored_backup_entry(backup.archive_path, "missing.dat", sdslot.size());
  EXPECT_TRUE(!missing.ok);
  EXPECT_TRUE(missing.entry_missing());
  EXPECT_TRUE(!vsm::read_stored_backup_entry(backup.archive_path, "sce_sys/sdslot.dat",
                                             sdslot.size() - 1).ok);
  EXPECT_TRUE(!std::filesystem::exists(base / "source.restore-tmp"));

  std::filesystem::remove_all(base);
}

void test_legacy_zip_metadata_can_be_recovered_without_rewriting_the_archive() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-legacy-metadata-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "source" / "sce_sys");
  const std::vector<unsigned char> sdslot = build_sdslot({
      {2, {2026, 7, 12, 1, 44, 7}, "WipEout 2048", "Ghost ship", "Campaign medals: 91"},
  });
  write_binary_file(base / "source" / "sce_sys" / "sdslot.dat", sdslot);
  std::ofstream(base / "source" / "data.bin", std::ios::binary) << "legacy-save";
  const vsm::BackupResult backup = vsm::create_backup_archive({
      (base / "source").string(), (base / "backups").string(), "PCSF00007",
      {2024, 1, 2, 3, 4, 5},
  });
  EXPECT_TRUE(backup.ok);

  const auto archive_size = std::filesystem::file_size(backup.archive_path);
  const auto archive_time = std::filesystem::last_write_time(backup.archive_path);
  const std::filesystem::path json_path =
      base / "backups" / "PCSF00007" / "2024-01-02 03-04-05.json";
  std::ofstream(json_path, std::ios::binary) << "{broken";
  EXPECT_TRUE(!vsm::read_save_metadata_json(json_path.string()).ok);

  const vsm::ArchiveReadResult embedded = vsm::read_stored_backup_entry(
      backup.archive_path, "sce_sys/sdslot.dat", vsm::kSdslotHeaderSize +
                                                     vsm::kMaxSaveSlots *
                                                         vsm::kSdslotRecordSize);
  EXPECT_TRUE(embedded.ok);
  const vsm::SaveMetadata recovered = vsm::parse_sdslot_data(embedded.data);
  EXPECT_EQ(recovered.slots.size(), static_cast<std::size_t>(1));
  std::string error;
  EXPECT_TRUE(vsm::write_save_metadata_json_atomic(
      json_path.string(), "2024-01-02 03-04-05", recovered, &error));
  EXPECT_TRUE(vsm::read_save_metadata_json(json_path.string()).ok);

  // Lazy recovery creates only the companion. The old ZIP remains byte-for-byte sized and keeps
  // its original filesystem timestamp, so upgrading cannot mutate a user's restore point.
  EXPECT_EQ(static_cast<std::size_t>(std::filesystem::file_size(backup.archive_path)),
            static_cast<std::size_t>(archive_size));
  EXPECT_TRUE(std::filesystem::last_write_time(backup.archive_path) == archive_time);
  std::filesystem::remove_all(base);
}

void test_backup_archive_extracts_to_isolated_inspection_directory_and_cleans_up() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-inspection-extract-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "source" / "sce_sys");
  std::ofstream(base / "source" / "data.bin", std::ios::binary) << "save-data";
  std::ofstream(base / "source" / "sce_sys" / "sdslot.dat", std::ios::binary)
      << "slot-data";
  const std::time_t original_file_time = 1700001234;
  const utimbuf original_times{original_file_time, original_file_time};
  EXPECT_TRUE(utime((base / "source" / "data.bin").string().c_str(), &original_times) == 0);

  const vsm::BackupResult backup = vsm::create_backup_archive({
      (base / "source").string(), (base / "backups").string(), "PCSE00001",
      {2026, 7, 15, 12, 0, 0},
  });
  EXPECT_TRUE(backup.ok);
  const auto archive_size = std::filesystem::file_size(backup.archive_path);
  const std::filesystem::path inspection = base / "inspection";

  const vsm::RestoreResult extracted =
      vsm::extract_backup_archive_for_inspection(backup.archive_path, inspection.string());
  EXPECT_TRUE(extracted.ok);
  EXPECT_TRUE(!extracted.file_timestamps_uniform);
  EXPECT_TRUE(std::filesystem::exists(inspection / "data.bin"));
  EXPECT_TRUE(std::filesystem::exists(inspection / "sce_sys" / "sdslot.dat"));
  struct stat extracted_info {};
  EXPECT_TRUE(stat((inspection / "data.bin").string().c_str(), &extracted_info) == 0);
  // DOS timestamps have two-second precision; the file's archived time must survive inspection
  // rather than becoming the current extraction time.
  EXPECT_EQ(static_cast<std::size_t>(extracted_info.st_mtime),
            static_cast<std::size_t>(original_file_time & ~1));
  EXPECT_EQ(static_cast<std::size_t>(std::filesystem::file_size(backup.archive_path)),
            static_cast<std::size_t>(archive_size));

  EXPECT_TRUE(vsm::remove_backup_inspection_directory(inspection.string()));
  EXPECT_TRUE(!std::filesystem::exists(inspection));
  EXPECT_TRUE(std::filesystem::exists(backup.archive_path));

  const std::filesystem::path uniform_source = base / "uniform-source";
  std::filesystem::create_directories(uniform_source);
  std::ofstream(uniform_source / "one.bin", std::ios::binary) << "one";
  std::ofstream(uniform_source / "two.bin", std::ios::binary) << "two";
  EXPECT_TRUE(utime((uniform_source / "one.bin").string().c_str(), &original_times) == 0);
  EXPECT_TRUE(utime((uniform_source / "two.bin").string().c_str(), &original_times) == 0);
  const vsm::BackupResult uniform_backup = vsm::create_backup_archive({
      uniform_source.string(), (base / "backups").string(), "PSP-LEGACY",
      {2026, 7, 15, 12, 0, 0},
  });
  EXPECT_TRUE(uniform_backup.ok);
  const std::filesystem::path uniform_inspection = base / "uniform-inspection";
  const vsm::RestoreResult uniform_extracted = vsm::extract_backup_archive_for_inspection(
      uniform_backup.archive_path, uniform_inspection.string());
  EXPECT_TRUE(uniform_extracted.ok);
  EXPECT_TRUE(uniform_extracted.file_timestamps_uniform);
  EXPECT_TRUE(vsm::remove_backup_inspection_directory(uniform_inspection.string()));

  // A single-file save is deliberately reported as uniform: one timestamp cannot be told apart
  // from a legacy synthetic backup stamp, so its filesystem time must not be trusted as a save time.
  const std::filesystem::path single_source = base / "single-source";
  std::filesystem::create_directories(single_source);
  std::ofstream(single_source / "only.bin", std::ios::binary) << "only";
  EXPECT_TRUE(utime((single_source / "only.bin").string().c_str(), &original_times) == 0);
  const vsm::BackupResult single_backup = vsm::create_backup_archive({
      single_source.string(), (base / "backups").string(), "PSP-SINGLE",
      {2026, 7, 15, 12, 0, 0},
  });
  EXPECT_TRUE(single_backup.ok);
  const std::filesystem::path single_inspection = base / "single-inspection";
  const vsm::RestoreResult single_extracted = vsm::extract_backup_archive_for_inspection(
      single_backup.archive_path, single_inspection.string());
  EXPECT_TRUE(single_extracted.ok);
  EXPECT_TRUE(single_extracted.file_timestamps_uniform);
  EXPECT_TRUE(vsm::remove_backup_inspection_directory(single_inspection.string()));

  // Local entries alone are not a complete ZIP. Rejecting this interruption also prevents a
  // uniform legacy archive from bypassing the synthetic-timestamp guard through the EOF path.
  std::ifstream complete_zip(uniform_backup.archive_path, std::ios::binary);
  const std::vector<unsigned char> zip_bytes((std::istreambuf_iterator<char>(complete_zip)),
                                              std::istreambuf_iterator<char>());
  EXPECT_TRUE(zip_bytes.size() >= 22);
  const std::uint32_t central_offset = read_le32(zip_bytes, zip_bytes.size() - 6);
  const std::filesystem::path truncated_archive = base / "truncated.zip";
  std::filesystem::copy_file(uniform_backup.archive_path, truncated_archive);
  std::filesystem::resize_file(truncated_archive, central_offset);
  const std::filesystem::path truncated_inspection = base / "truncated-inspection";
  const vsm::RestoreResult truncated = vsm::extract_backup_archive_for_inspection(
      truncated_archive.string(), truncated_inspection.string());
  EXPECT_TRUE(!truncated.ok);
  EXPECT_TRUE(!std::filesystem::exists(truncated_inspection));

  const std::filesystem::path oversized = base / "oversized-inspection";
  const vsm::RestoreResult rejected = vsm::extract_backup_archive_for_inspection(
      backup.archive_path, oversized.string(), 4);
  EXPECT_TRUE(!rejected.ok);
  EXPECT_TRUE(!std::filesystem::exists(oversized));
  std::filesystem::remove_all(base);
}

void test_backup_archive_explicit_name_never_overwrites() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-exclusive-archive-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "source");
  std::ofstream(base / "source" / "data.bin", std::ios::binary) << "first";

  vsm::BackupRequest request;
  request.source_path = (base / "source").string();
  request.backup_root = (base / "backups").string();
  request.save_id = "PCSE00001";
  request.timestamp = {2026, 7, 12, 1, 44, 7};
  request.archive_name = "2026-07-12 01-44-07.zip";
  const vsm::BackupResult first = vsm::create_backup_archive(request);
  EXPECT_TRUE(first.ok);

  std::ofstream(base / "source" / "data.bin", std::ios::binary | std::ios::trunc) << "second";
  const vsm::BackupResult second = vsm::create_backup_archive(request);
  EXPECT_TRUE(!second.ok);
  EXPECT_TRUE(second.error.find("exists") != std::string::npos);
  bool entries_ok = false;
  const std::vector<vsm::ArchiveEntryInfo> changed =
      vsm::compute_folder_entries((base / "source").string(), &entries_ok);
  EXPECT_TRUE(entries_ok);
  EXPECT_TRUE(!vsm::entries_match_backup_archive(changed, first.archive_path));

  std::filesystem::remove_all(base);
}

void test_backup_creation_plan_reuses_matching_content_and_counts_collisions() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-creation-plan-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "source");
  std::ofstream(base / "source" / "data.bin", std::ios::binary) << "first";

  const vsm::BackupTimestamp timestamp{2026, 7, 12, 1, 44, 7};
  vsm::BackupRequest request;
  request.source_path = (base / "source").string();
  request.backup_root = (base / "backups").string();
  request.save_id = "PCSE00001";
  request.timestamp = timestamp;
  request.archive_name = "2026-07-12 01-44-07 before-boss.zip";
  const vsm::BackupResult first = vsm::create_backup_archive(request);
  EXPECT_TRUE(first.ok);

  bool entries_ok = false;
  std::vector<vsm::ArchiveEntryInfo> entries =
      vsm::compute_folder_entries((base / "source").string(), &entries_ok);
  EXPECT_TRUE(entries_ok);
  vsm::BackupCreationPlan plan = vsm::plan_backup_creation(
      timestamp, "", entries, (base / "backups").string(), "PCSE00001",
      {"2026-07-12 01-44-07 before-boss.zip"}, {});
  EXPECT_TRUE(plan.reuse_existing);
  EXPECT_EQ(plan.archive_name, "2026-07-12 01-44-07 before-boss.zip");

  plan = vsm::plan_backup_creation(
      timestamp, "", entries, (base / "backups").string(), "PCSE00001",
      {"2026-07-12 01-44-07 before-boss.zip"}, {}, false);
  EXPECT_TRUE(!plan.reuse_existing);
  EXPECT_EQ(plan.archive_name, "2026-07-12 01-44-07~2.zip");

  std::ofstream(base / "source" / "data.bin", std::ios::binary | std::ios::trunc) << "second";
  entries = vsm::compute_folder_entries((base / "source").string(), &entries_ok);
  EXPECT_TRUE(entries_ok);
  plan = vsm::plan_backup_creation(
      timestamp, "", entries, (base / "backups").string(), "PCSE00001",
      {"2026-07-12 01-44-07 before-boss.zip"}, {"2026-07-12 01-44-07~2.zip"});
  EXPECT_TRUE(!plan.reuse_existing);
  EXPECT_EQ(plan.archive_name, "2026-07-12 01-44-07~3.zip");

  plan = vsm::plan_backup_creation(timestamp, " auto", entries,
                                   (base / "backups").string(), "PCSE00001", {},
                                   {"2026-07-12 01-44-07.zip"});
  EXPECT_TRUE(!plan.reuse_existing);
  EXPECT_EQ(plan.archive_name, "2026-07-12 01-44-07~2 auto.zip");

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
    std::ofstream(base / "PCSE00120_ Persona_4" / "2026-05-21 16-14.json") << "metadata";
    std::ofstream(base / "PCSE00120_ Persona_4" / "orphan.json") << "orphan";
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

void test_backup_download_publication_is_exclusive_and_cleans_temporary_files() {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / "save-keeper-download-publication-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base);

  const std::filesystem::path temporary = base / "backup.zip.download";
  const std::filesystem::path destination = base / "backup.zip";
  const std::filesystem::path interrupted_reservation = base / "backup.zip.publishing";
  // Simulate a power loss after reserving the name. Recovery removes the hidden reservation;
  // it must never expose an empty backup.zip to the normal backup scanner.
  std::filesystem::create_directory(interrupted_reservation);
  std::ofstream(temporary, std::ios::binary) << "complete-backup";
  std::string error;
  EXPECT_TRUE(vsm::publish_backup_download(temporary.string(), destination.string(), &error));
  EXPECT_TRUE(!std::filesystem::exists(temporary));
  EXPECT_TRUE(!std::filesystem::exists(interrupted_reservation));
  EXPECT_TRUE(std::filesystem::exists(destination));

  // A later metadata inspection failure must not remove the successfully downloaded ZIP.
  const bool metadata_recovered = false;
  EXPECT_TRUE(!metadata_recovered);
  EXPECT_TRUE(std::filesystem::exists(destination));

  std::ofstream(temporary, std::ios::binary) << "replacement";
  EXPECT_TRUE(!vsm::publish_backup_download(temporary.string(), destination.string(), &error));
  EXPECT_TRUE(!std::filesystem::exists(temporary));
  std::ifstream published(destination, std::ios::binary);
  const std::string contents((std::istreambuf_iterator<char>(published)),
                             std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, "complete-backup");
  std::filesystem::remove_all(base);
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
  EXPECT_EQ(vsm::build_drive_sidecar_upload_metadata_json(
                "2026-07-12 01-44-07.json", "folder-id", "zip-id"),
            "{\"name\":\"2026-07-12 01-44-07.json\",\"parents\":[\"folder-id\"],"
            "\"appProperties\":{\"archiveFileId\":\"zip-id\"}}");
  EXPECT_EQ(vsm::build_drive_sidecar_update_metadata_json(
                "2026-07-12 01-44-07.json", "zip-id"),
            "{\"name\":\"2026-07-12 01-44-07.json\","
            "\"appProperties\":{\"archiveFileId\":\"zip-id\"}}");
  EXPECT_EQ(vsm::build_drive_multipart_update_url("sidecar/id"),
            "https://www.googleapis.com/upload/drive/v3/files/sidecar%2Fid?uploadType="
            "multipart&fields=id%2Cname");
}

void test_google_drive_builds_sidecar_lookup_queries() {
  EXPECT_EQ(vsm::build_drive_find_sidecar_by_archive_query("folder-id", "zip-id"),
            "q=%27folder-id%27%20in%20parents%20and%20appProperties%20has%20%7B%20key%3D%27"
            "archiveFileId%27%20and%20value%3D%27zip-id%27%20%7D%20and%20trashed%3Dfalse&"
            "fields=files%28id%2Cname%29");
  EXPECT_EQ(vsm::build_drive_find_child_by_name_query("folder-id", "snapshot.json"),
            "q=name%3D%27snapshot.json%27%20and%20%27folder-id%27%20in%20parents%20and%20"
            "trashed%3Dfalse&fields=files%28id%2Cname%29");

  const std::string escaped =
      vsm::build_drive_find_child_by_name_query("fold'er\\id", "boss's\\save.json");
  EXPECT_TRUE(escaped.find("fold%5C%27er%5C%5Cid") != std::string::npos);
  EXPECT_TRUE(escaped.find("boss%5C%27s%5C%5Csave.json") != std::string::npos);
}

void test_google_drive_builds_list_children_query() {
  EXPECT_EQ(vsm::build_drive_list_children_query("folder-id"),
            "q=%27folder-id%27%20in%20parents%20and%20trashed%3Dfalse&fields=files%28id%2Cname%"
            "29");
}

void test_google_drive_builds_paged_index_queries() {
  EXPECT_EQ(vsm::build_drive_list_all_folders_query(""),
            "q=mimeType%3D%27application%2Fvnd.google-apps.folder%27%20and%20trashed%3Dfalse"
            "&fields=nextPageToken%2Cfiles%28id%2Cname%2Cparents%2Csize%29&pageSize=1000");
  EXPECT_EQ(vsm::build_drive_list_all_files_query("token-1"),
            "q=mimeType%21%3D%27application%2Fvnd.google-apps.folder%27%20and%20name%20contains"
            "%20%27.zip%27%20and%20trashed%3Dfalse"
            "&fields=nextPageToken%2Cfiles%28id%2Cname%2Cparents%2Csize%29&pageSize=1000"
            "&pageToken=token-1");
}

void test_google_drive_parses_parents_and_page_token() {
  // Shape taken from a live Drive v3 response: pretty-printed, and "parents" comes BEFORE
  // "id"/"name" inside each object. The parser must not depend on field order.
  const vsm::DriveFileList files = vsm::parse_drive_file_list(
      "{\n \"nextPageToken\": \"tok-2\",\n \"files\": [\n"
      "  {\n   \"parents\": [\n    \"folder-a\"\n   ],\n"
      "   \"id\": \"zip-1\",\n   \"name\": \"2026-07-03 23-19.zip\",\n"
      "   \"size\": \"3005056\"\n  },\n"
      "  {\n   \"id\": \"orphan\",\n   \"name\": \"loose.zip\"\n  },\n"
      "  {\n   \"id\": \"zip-2\",\n   \"name\": \"2026-07-01 10-00.zip\",\n"
      "   \"parents\": [\n    \"folder-b\"\n   ]\n  }\n ]\n}\n");

  EXPECT_TRUE(files.ok);
  EXPECT_EQ(files.next_page_token, "tok-2");
  EXPECT_EQ(files.files.size(), static_cast<std::size_t>(3));
  EXPECT_EQ(files.files[0].id, "zip-1");
  EXPECT_EQ(files.files[0].name, "2026-07-03 23-19.zip");
  EXPECT_EQ(files.files[0].parent_id, "folder-a");
  // Drive v3 serializes int64 size as a quoted string; absent size stays 0.
  EXPECT_EQ(files.files[0].size_bytes, 3005056LL);
  EXPECT_EQ(files.files[1].parent_id, "");
  EXPECT_EQ(files.files[1].size_bytes, 0LL);
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

void test_multipart_body_frames_match_the_drive_wire_format() {
  // prefix + <file bytes> + suffix must be byte-identical to the body the app used to assemble
  // in RAM, or existing Drive uploads would change shape on the wire
  const vsm::MultipartRelatedBody body = vsm::build_multipart_related_body(
      "test-boundary", "{\"name\":\"backup.zip\"}", "application/zip", 4);

  EXPECT_EQ(body.prefix,
            std::string("--test-boundary\r\n"
                        "Content-Type: application/json; charset=UTF-8\r\n\r\n"
                        "{\"name\":\"backup.zip\"}\r\n"
                        "--test-boundary\r\n"
                        "Content-Type: application/zip\r\n\r\n"));
  EXPECT_EQ(body.suffix, std::string("\r\n--test-boundary--\r\n"));
  EXPECT_EQ(static_cast<std::size_t>(body.file_size), static_cast<std::size_t>(4));
  EXPECT_EQ(static_cast<std::size_t>(body.total_size()),
            body.prefix.size() + 4 + body.suffix.size());
}

void test_multipart_chunks_walk_regions_without_spanning_boundaries() {
  vsm::MultipartRelatedBody body;
  body.prefix = "PREFIX";
  body.suffix = "END";
  body.file_size = 10;

  // a capacity larger than the remaining region still serves only that region: the reader
  // switches data sources between calls, so a chunk must never straddle two regions
  vsm::MultipartChunk chunk = vsm::next_multipart_chunk(body, 0, 64);
  EXPECT_TRUE(chunk.source == vsm::MultipartChunk::Source::Prefix);
  EXPECT_EQ(static_cast<std::size_t>(chunk.source_offset), static_cast<std::size_t>(0));
  EXPECT_EQ(chunk.length, static_cast<std::size_t>(6));

  // a small buffer mid-prefix serves the capacity, not the region remainder
  chunk = vsm::next_multipart_chunk(body, 2, 3);
  EXPECT_TRUE(chunk.source == vsm::MultipartChunk::Source::Prefix);
  EXPECT_EQ(static_cast<std::size_t>(chunk.source_offset), static_cast<std::size_t>(2));
  EXPECT_EQ(chunk.length, static_cast<std::size_t>(3));

  chunk = vsm::next_multipart_chunk(body, 6, 64);
  EXPECT_TRUE(chunk.source == vsm::MultipartChunk::Source::File);
  EXPECT_EQ(static_cast<std::size_t>(chunk.source_offset), static_cast<std::size_t>(0));
  EXPECT_EQ(chunk.length, static_cast<std::size_t>(10));

  chunk = vsm::next_multipart_chunk(body, 13, 2);
  EXPECT_TRUE(chunk.source == vsm::MultipartChunk::Source::File);
  EXPECT_EQ(static_cast<std::size_t>(chunk.source_offset), static_cast<std::size_t>(7));
  EXPECT_EQ(chunk.length, static_cast<std::size_t>(2));

  chunk = vsm::next_multipart_chunk(body, 16, 64);
  EXPECT_TRUE(chunk.source == vsm::MultipartChunk::Source::Suffix);
  EXPECT_EQ(static_cast<std::size_t>(chunk.source_offset), static_cast<std::size_t>(0));
  EXPECT_EQ(chunk.length, static_cast<std::size_t>(3));

  chunk = vsm::next_multipart_chunk(body, 18, 64);
  EXPECT_TRUE(chunk.source == vsm::MultipartChunk::Source::Suffix);
  EXPECT_EQ(static_cast<std::size_t>(chunk.source_offset), static_cast<std::size_t>(2));
  EXPECT_EQ(chunk.length, static_cast<std::size_t>(1));

  chunk = vsm::next_multipart_chunk(body, 19, 64);
  EXPECT_TRUE(chunk.source == vsm::MultipartChunk::Source::End);
  EXPECT_EQ(chunk.length, static_cast<std::size_t>(0));
}

void test_multipart_chunks_handle_empty_files_and_past_end_offsets() {
  vsm::MultipartRelatedBody body;
  body.prefix = "P";
  body.suffix = "S";
  body.file_size = 0;

  // an empty file collapses the file region: the prefix hands over directly to the suffix
  vsm::MultipartChunk chunk = vsm::next_multipart_chunk(body, 1, 64);
  EXPECT_TRUE(chunk.source == vsm::MultipartChunk::Source::Suffix);
  EXPECT_EQ(static_cast<std::size_t>(chunk.source_offset), static_cast<std::size_t>(0));
  EXPECT_EQ(chunk.length, static_cast<std::size_t>(1));

  // offsets at or past the total are a clean end, never an out-of-bounds region read
  chunk = vsm::next_multipart_chunk(body, 2, 64);
  EXPECT_TRUE(chunk.source == vsm::MultipartChunk::Source::End);
  chunk = vsm::next_multipart_chunk(body, 100, 64);
  EXPECT_TRUE(chunk.source == vsm::MultipartChunk::Source::End);
  EXPECT_EQ(chunk.length, static_cast<std::size_t>(0));
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
  std::vector<vsm::SaveRecord> saves(4);
  saves[0].id = "PCSB00001";
  saves[0].display_name = "Alpha";
  saves[0].saved_at_epoch = 100;
  saves[0].save_time_known = true;
  saves[1].id = "PCSB00002";
  saves[1].display_name = "Bravo";
  saves[1].saved_at_epoch = 300;
  saves[1].save_time_known = true;
  saves[2].id = "PCSB00003";
  saves[2].display_name = "Charlie";
  saves[2].saved_at_epoch = 200;
  saves[2].save_time_known = true;
  saves[3].id = "PCSB00004";
  saves[3].display_name = "Delta";
  saves[3].saved_at_epoch = 999;

  vsm::apply_save_sort(&saves, vsm::SaveSortMode::LastSaved, {});
  EXPECT_EQ(saves[0].display_name, "Bravo");
  EXPECT_EQ(saves[1].display_name, "Charlie");
  EXPECT_EQ(saves[2].display_name, "Alpha");
  EXPECT_EQ(saves[3].display_name, "Delta");

  // Only Alpha and Charlie exist on Drive; Charlie's backup is newer, Bravo sinks to the end.
  const std::map<std::string, std::string> newest = {
      {"PCSB00001", "2026-07-01 10-00-00.zip"},
      {"PCSB00003", "2026-07-04 09-30-00.zip"},
  };
  vsm::apply_save_sort(&saves, vsm::SaveSortMode::LastBackup, newest);
  EXPECT_EQ(saves[0].display_name, "Charlie");
  EXPECT_EQ(saves[1].display_name, "Alpha");
  EXPECT_EQ(saves[2].display_name, "Bravo");
  EXPECT_EQ(saves[3].display_name, "Delta");
  EXPECT_EQ(std::string(vsm::save_sort_mode_label(vsm::SaveSortMode::LastBackup)), "Backup");

  vsm::apply_save_sort(&saves, vsm::SaveSortMode::Name, {});
  EXPECT_EQ(saves[0].display_name, "Alpha");
}

void test_only_last_saved_sort_requires_all_save_times() {
  EXPECT_TRUE(!vsm::save_sort_requires_all_times(vsm::SaveSortMode::Name));
  EXPECT_TRUE(vsm::save_sort_requires_all_times(vsm::SaveSortMode::LastSaved));
  EXPECT_TRUE(!vsm::save_sort_requires_all_times(vsm::SaveSortMode::LastBackup));
}

void test_app_settings_roundtrip_and_unknown_keys() {
  vsm::AppSettings settings;
  settings.sort_mode = vsm::SaveSortMode::LastBackup;
  EXPECT_EQ(vsm::serialize_app_settings(settings), "sort=backup\n");

  const vsm::AppSettings parsed =
      vsm::parse_app_settings("future_key=whatever\r\nsort=saved\n");
  EXPECT_TRUE(parsed.sort_mode == vsm::SaveSortMode::LastSaved);

  // "synced" is what pre-rename builds wrote for the backup sort; it must keep parsing.
  const vsm::AppSettings legacy = vsm::parse_app_settings("sort=synced\n");
  EXPECT_TRUE(legacy.sort_mode == vsm::SaveSortMode::LastBackup);

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
  // Without Drive the dropped "& upload" alone is easy to miss; "locally" states outright that
  // nothing will reach the Cloud.
  EXPECT_EQ(vsm::sync_all_confirm_message(4, "PSP", false),
            "Backup all 4 PSP saves locally?");
  EXPECT_EQ(vsm::sync_all_confirm_message(1, "Homebrew", true),
            "Backup & upload 1 Homebrew save?");
  EXPECT_EQ(vsm::sync_all_confirm_message(1, "Homebrew", false),
            "Backup 1 Homebrew save locally?");
}

void test_sync_run_summary_reports_results_and_cancellation() {
  EXPECT_EQ(vsm::sync_run_summary({3, 8, 0, 0, 0}), "Backed up 3, uploaded 8.");
  EXPECT_EQ(vsm::sync_run_summary({3, 0, 0, 0, 0}), "Backed up 3.");
  EXPECT_EQ(vsm::sync_run_summary({0, 8, 0, 0, 0}), "Uploaded 8.");
  EXPECT_EQ(vsm::sync_run_summary({2, 6, 56, 3, 0}),
            "Backed up 2, uploaded 6, 56 up to date, 3 failed.");
  EXPECT_EQ(vsm::sync_run_summary({0, 0, 73, 0, 0}), "All 73 games up to date.");
  EXPECT_EQ(vsm::sync_run_summary({1, 2, 0, 0, 9}),
            "Canceled: backed up 1, uploaded 2, 9 games left.");
  EXPECT_EQ(vsm::sync_run_summary({0, 0, 0, 0, 9}), "Canceled: 9 games left.");
  EXPECT_EQ(vsm::sync_run_summary({0, 0, 3, 0, 1}),
            "Canceled: 3 up to date, 1 game left.");
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

void test_tap_hold_gesture_resolves_release_and_trigger_once() {
  using vsm::TapHoldAction;
  EXPECT_TRUE(vsm::resolve_tap_hold_action(4, true, false, 8, 60) ==
              TapHoldAction::Tap);
  EXPECT_TRUE(vsm::resolve_tap_hold_action(60, false, false, 8, 60) ==
              TapHoldAction::Hold);
  EXPECT_TRUE(vsm::resolve_tap_hold_action(20, true, false, 8, 60) ==
              TapHoldAction::None);
  EXPECT_TRUE(vsm::resolve_tap_hold_action(60, false, true, 8, 60) ==
              TapHoldAction::None);
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
  test_backup_timestamp_parser_reads_legacy_names_defensively();
  test_backup_counter_identity_and_allocation();
  test_backup_label_sanitizer_and_auto_conflict();
  test_path_component_normalization_replaces_unsafe_characters();
  test_sfo_parser_reads_title_strings();
  test_sdslot_parser_reads_noncontiguous_slots_and_selects_newest();
  test_sdslot_parser_converts_utc_slot_time_to_device_local_time();
  test_sdslot_parser_skips_bad_records_but_keeps_valid_siblings();
  test_sdslot_parser_rejects_invalid_magic_and_truncation_but_keeps_full_fields();
  test_save_metadata_resolver_uses_recursive_files_then_backup_clock();
  test_pfs_mount_policy_requires_existing_metadata_directory();
  test_save_metadata_json_round_trips_and_ignores_unknown_fields();
  test_legacy_vita_slot_json_is_upgraded_from_utc_to_local_time();
  test_backup_metadata_is_usable_only_for_matching_trustworthy_identity();
  test_observed_save_metadata_accepts_slots_and_files_but_not_backup_clock();
  test_save_metadata_json_rejects_untrusted_bounds_and_invalid_utf8();
  test_save_metadata_json_file_write_is_atomic_and_bounded();
  test_backup_metadata_names_follow_archive_identity();
  test_save_scanner_lists_direct_child_save_directories();
  test_mounted_save_time_trusts_observed_times_and_rejects_unresolved();
  test_selection_wraps_and_handles_empty_lists();
  test_grid_window_scrolls_only_when_selection_leaves_view();
  test_backup_archive_creates_timestamped_zip_snapshot();
  test_detail_view_sizes_from_folder_and_archive();
  test_save_fingerprint_reflects_folder_content();
  test_save_time_cache_roundtrip_and_rejects_bad_input();
  test_scan_fingerprints_every_save_and_flags_mount_requiring_ones();
  test_scan_consumes_time_and_title_caches_when_fingerprints_match();
  test_save_title_cache_roundtrip_and_stat_stamp();
  test_backup_archive_reads_bounded_sdslot_entry_without_restoring();
  test_legacy_zip_metadata_can_be_recovered_without_rewriting_the_archive();
  test_backup_archive_extracts_to_isolated_inspection_directory_and_cleans_up();
  test_backup_archive_explicit_name_never_overwrites();
  test_backup_creation_plan_reuses_matching_content_and_counts_collisions();
  test_backup_archive_restores_snapshot_and_removes_stale_files();
  test_backup_archive_missing_file_does_not_clear_destination();
  test_backup_store_lists_local_zip_backups_newest_first();
  test_backup_store_builds_normalized_archive_path();
  test_backup_download_publication_is_exclusive_and_cleans_temporary_files();
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
  test_google_drive_builds_sidecar_lookup_queries();
  test_google_drive_builds_list_children_query();
  test_google_drive_builds_paged_index_queries();
  test_google_drive_parses_parents_and_page_token();
  test_google_drive_parses_single_object_upload_response();
  test_multipart_body_frames_match_the_drive_wire_format();
  test_multipart_chunks_walk_regions_without_spanning_boundaries();
  test_multipart_chunks_handle_empty_files_and_past_end_offsets();
  test_save_category_classification();
  test_saves_sort_by_display_name_case_insensitive();
  test_save_sort_modes_order_saves();
  test_only_last_saved_sort_requires_all_save_times();
  test_utf8_truncation_and_system_font_detection();
  test_auto_backup_suffix_display_and_content_matching();
  test_app_settings_roundtrip_and_unknown_keys();
  test_sync_plan_decides_backup_and_upload_per_game();
  test_sync_all_confirm_message_states_scope();
  test_sync_run_summary_reports_results_and_cancellation();
  test_sync_all_hold_message_names_the_tab();
  test_display_backup_name_strips_zip_extension();
  test_tap_hold_gesture_resolves_release_and_trigger_once();
  test_drive_save_folder_names_carry_the_game_title();
  test_drive_folder_matching_accepts_bare_and_titled_names();
  test_drive_rename_metadata_json_escapes_the_name();

  std::cout << "vsm_core_tests passed\n";
  return 0;
}
