#include "core/BackupList.hpp"
#include "core/BackupName.hpp"
#include "core/PathUtil.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
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

} // namespace

int main() {
  test_timestamped_backup_name_uses_jksv_style_zip_name();
  test_remote_entries_are_prefixed_and_local_entries_are_plain();
  test_backup_menu_order_matches_jksv_style();
  test_path_component_normalization_replaces_unsafe_characters();

  std::cout << "vsm_core_tests passed\n";
  return 0;
}
