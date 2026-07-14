#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace vsm {

enum class SaveTimeSource {
  VitaSlot,
  Filesystem,
  BackupClock,
};

struct SaveDateTime {
  int year{};
  int month{};
  int day{};
  int hour{};
  int minute{};
  int second{};
};

struct SaveSlotMetadata {
  unsigned int id{};
  SaveDateTime modified_at;
  std::string title;
  std::string subtitle;
  std::string details;
};

struct SaveMetadata {
  SaveDateTime saved_at;
  SaveTimeSource source{SaveTimeSource::BackupClock};
  bool approximate{true};
  std::vector<SaveSlotMetadata> slots;
};

constexpr std::size_t kSdslotHeaderSize = 0x400;
constexpr std::size_t kSdslotRecordSize = 0x400;
constexpr std::size_t kMaxSaveSlots = 256;

SaveMetadata parse_sdslot_data(const std::vector<unsigned char> &data);
SaveMetadata resolve_save_metadata(const std::string &save_path,
                                   const SaveDateTime &backup_clock);
SaveDateTime current_local_datetime();
std::string format_save_datetime(const SaveDateTime &value);
long long save_datetime_to_local_epoch(const SaveDateTime &value);

} // namespace vsm
